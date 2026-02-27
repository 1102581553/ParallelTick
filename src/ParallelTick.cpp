#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/Config.h>
#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/BlockSource.h>
#include <mc/deps/ecs/gamerefs_entity/EntityContext.h>
#include <mc/deps/ecs/WeakEntityRef.h>
#include <mc/legacy/ActorRuntimeID.h>
#include <windows.h>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <cstdio>
#include <exception>
#include <malloc.h>
#include <array>

using LRA = ::OwnerPtr<::EntityContext>(Level::*)(::Actor&);
using LRW = ::OwnerPtr<::EntityContext>(Level::*)(::WeakEntityRef);

static LONG sehFilter(unsigned int c){return(c==0xE06D7363u)?EXCEPTION_CONTINUE_SEARCH:EXCEPTION_EXECUTE_HANDLER;}
static int tickSEH(Actor*a,BlockSource&r){__try{a->tick(r);return 0;}__except(sehFilter(GetExceptionCode())){DWORD c=GetExceptionCode();if(c==EXCEPTION_STACK_OVERFLOW)_resetstkoflw();return(int)c;}}
static int tickSafe(Actor*a,BlockSource&r){try{return tickSEH(a,r);}catch(const std::exception&e){fprintf(stderr,"[PT]C++ %p:%s\n",(void*)a,e.what());return-1;}catch(...){fprintf(stderr,"[PT]Unk %p\n",(void*)a);return-2;}}

namespace parallel_tick {

static std::string nowStr(){auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());tm b;localtime_s(&b,&t);std::ostringstream s;s<<std::put_time(&b,"%H:%M:%S");return s.str();}
static Actor* weakToActor(WeakEntityRef const&ref){if(auto s=ref.lock())if(auto*c=s.operator->())return Actor::tryGetFromEntity(*c,false);return nullptr;}

ParallelTick& ParallelTick::getInstance(){static ParallelTick i;return i;}

void ParallelTick::startStatsTask(){
    if(mStatsRunning.exchange(true))return;
    ll::coro::keepThis([this]()->ll::coro::CoroTask<>{
        while(mStatsRunning.load()){
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this]{
                if(!mConfig.stats)return;
                size_t t=mStatTotal.exchange(0),k=mStatTasks.exchange(0),c=mCrashCount.load();
                if(t>0)getSelf().getLogger().info("[{}][Stats] E={} T={} C={}",nowStr(),t,k,c);
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}
void ParallelTick::stopStatsTask(){mStatsRunning.store(false);}

bool ParallelTick::load(){
    auto p=getSelf().getConfigDir()/"config.json";
    if(!ll::config::loadConfig(mConfig,p))ll::config::saveConfig(mConfig,p);
    getSelf().getLogger().info("[{}][load] en={} thr={} grid={} maxE={}",nowStr(),mConfig.enabled,mConfig.threadCount,mConfig.gridSizeBase,mConfig.maxEntitiesPerTask);
    return true;
}
bool ParallelTick::enable(){
    int n=mConfig.threadCount;
    if(n<=0)n=std::max(1u,std::thread::hardware_concurrency()-1);
    mPool=std::make_unique<FixedThreadPool>(n,8*1024*1024);
    registerHooks();
    getSelf().getLogger().info("[{}][enable] threads={}",nowStr(),n);
    if(mConfig.stats||mConfig.debug)startStatsTask();
    return true;
}
bool ParallelTick::disable(){
    unregisterHooks();stopStatsTask();mPool.reset();
    getSelf().getLogger().info("[{}][disable]",nowStr());
    return true;
}

// ═══ Hook: addEntity ═══
LL_TYPE_INSTANCE_HOOK(HookAddEntity,ll::memory::HookPriority::Normal,Level,&Level::$addEntity,::Actor*,::BlockSource&bs,::OwnerPtr<::EntityContext>ep){
    auto&pt=ParallelTick::getInstance();
    if(pt.isParallelPhase()){std::lock_guard<std::mutex>lk(GlobalLocks::get().entityLifecycleLock);return origin(bs,std::move(ep));}
    return origin(bs,std::move(ep));
}

// ═══ Hook: removeEntity x2 ═══
LL_TYPE_INSTANCE_HOOK(HookRemoveActor,ll::memory::HookPriority::Normal,Level,static_cast<LRA>(&Level::$removeEntity),::OwnerPtr<::EntityContext>,::Actor&actor){
    auto&pt=ParallelTick::getInstance();
    pt.onActorRemoved(&actor);
    if(pt.isParallelPhase()){std::lock_guard<std::mutex>lk(GlobalLocks::get().entityLifecycleLock);return origin(actor);}
    return origin(actor);
}
LL_TYPE_INSTANCE_HOOK(HookRemoveWeak,ll::memory::HookPriority::Normal,Level,static_cast<LRW>(&Level::$removeEntity),::OwnerPtr<::EntityContext>,::WeakEntityRef ref){
    auto&pt=ParallelTick::getInstance();
    if(Actor*a=weakToActor(ref))pt.onActorRemoved(a);
    if(pt.isParallelPhase()){std::lock_guard<std::mutex>lk(GlobalLocks::get().entityLifecycleLock);return origin(std::move(ref));}
    return origin(std::move(ref));
}

// ═══ Hook: Actor::tick — 收集 ═══
LL_TYPE_INSTANCE_HOOK(HookActorTick,ll::memory::HookPriority::Normal,Actor,&Actor::tick,bool,::BlockSource&region){
    auto&pt=ParallelTick::getInstance();
    if(!pt.getConfig().enabled||!pt.isCollecting())return origin(region);
    if(this->isPlayer()||this->isSimulatedPlayer())return origin(region);
    if(pt.isPermanentlyCrashed(this))return true;
    pt.collectActor(this,region);
    return true;
}

// ═══ Hook: Level::$tick — 4色棋盘分相并行 ═══
LL_TYPE_INSTANCE_HOOK(HookLevelTick,ll::memory::HookPriority::Normal,Level,&Level::$tick,void){
    static thread_local bool guard=false;
    if(guard){origin();return;}
    guard=true;

    auto&pt=ParallelTick::getInstance();
    const auto&cfg=pt.getConfig();
    auto t0=std::chrono::steady_clock::now();

    if(!cfg.enabled){origin();guard=false;return;}

    // 收集
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    auto list=pt.takeQueue();
    if(list.empty()){pt.clearAll();guard=false;return;}

    // 过滤
    std::vector<ActorTickEntry> snap;
    snap.reserve(list.size());
    for(auto&e:list)if(e.actor&&pt.isActorSafeToTick(e.actor))snap.push_back(e);
    if(snap.empty()){pt.clearAll();guard=false;return;}

    // BS分组 → 网格 → 4色
    struct Task{BlockSource*bs;std::vector<ActorTickEntry>ents;};
    std::array<std::vector<Task>,4> phases;
    std::unordered_map<BlockSource*,std::vector<ActorTickEntry>> bsMap;
    for(auto&e:snap)bsMap[e.region].push_back(e);

    size_t taskCount=0;
    int mx=cfg.maxEntitiesPerTask;
    for(auto&[bs,ents]:bsMap){
        std::unordered_map<GridPos,std::vector<ActorTickEntry>,GridPosHash> grid;
        for(auto&e:ents){
            auto p=e.actor->getPosition();
            GridPos gp{(int)std::floor(p.x/cfg.gridSizeBase),(int)std::floor(p.z/cfg.gridSizeBase)};
            grid[gp].push_back(e);
        }
        for(auto&[gp,ge]:grid){
            int color=gridColor(gp);
            if((int)ge.size()<=mx){
                phases[color].push_back({bs,std::move(ge)});
                taskCount++;
            }else{
                for(size_t i=0;i<ge.size();i+=(size_t)mx){
                    size_t end=std::min(i+(size_t)mx,ge.size());
                    phases[color].push_back({bs,{ge.begin()+i,ge.begin()+end}});
                    taskCount++;
                }
            }
        }
    }

    pt.addStats(snap.size(),taskCount);
    if(cfg.debug)pt.getSelf().getLogger().info("[{}] {} ents {} tasks [{},{},{},{}]",nowStr(),snap.size(),taskCount,phases[0].size(),phases[1].size(),phases[2].size(),phases[3].size());

    // 逐相位并行
    pt.setParallelPhase(true);
    auto&pool=pt.getPool();
    auto timeout=std::chrono::milliseconds(cfg.actorTickTimeoutMs>0?cfg.actorTickTimeoutMs:30000);

    for(int c=0;c<4;++c){
        if(phases[c].empty())continue;
        for(auto&task:phases[c]){
            pool.submit([&pt,&cfg,bs=task.bs,ents=std::move(task.ents)](){
                for(auto&e:ents){
                    if(!pt.isActorSafeToTick(e.actor))continue;
                    if(!e.actor->isInWorld())continue;
                    int ex=tickSafe(e.actor,*bs);
                    if(ex!=0){
                        pt.markCrashed(e.actor);
                        pt.getSelf().getLogger().error("[{}][Crash] 0x{:08X} actor={:p} type={} frozen",nowStr(),(unsigned)(ex&0xFFFFFFFF),(void*)e.actor,(int)e.actor->getEntityTypeId());
                    }
                }
            });
        }
        if(!pool.waitAllFor(timeout)){
            pt.getSelf().getLogger().error("[{}][Phase {}] TIMEOUT {}ms pend={}",nowStr(),c,timeout.count(),pool.pendingCount());
            pool.waitAllFor(timeout*2);
        }
    }

    pt.setParallelPhase(false);
    pt.clearAll();

    if(cfg.debug){
        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
        pt.getSelf().getLogger().info("[{}][Tick] {}ms",nowStr(),ms);
    }
    guard=false;
}

// ═══ 注册/注销 ═══
void registerHooks(){HookAddEntity::hook();HookRemoveActor::hook();HookRemoveWeak::hook();HookActorTick::hook();HookLevelTick::hook();}
void unregisterHooks(){HookAddEntity::unhook();HookRemoveActor::unhook();HookRemoveWeak::unhook();HookActorTick::unhook();HookLevelTick::unhook();}

LL_REGISTER_MOD(parallel_tick::ParallelTick,parallel_tick::ParallelTick::getInstance());

} // namespace parallel_tick
