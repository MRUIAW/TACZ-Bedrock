#include "nbt/StackLimitProbe.h"
#include <atomic>
#include <array>
#include <chrono>
#include <mutex>
#include <type_traits>
#include <vector>
#include <Windows.h>
#include <intrin.h>
#include "ll/api/memory/Hook.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStackBase.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemDescriptor.h"
#include "mc/world/item/InternalItemDescriptor.h"
#include "mc/world/item/components/ComponentItem.h"
#include "mc/deps/core/string/HashedString.h"
#include "nbt/AmmoItem.h"
#include "mod/TaczMod.h"
#include "pack/DescriptorProbe.h"
#include "pack/PackItems.h"
#ifdef TACZ_CLIENT
#include "client/gameplay/ClientShooter.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#endif

namespace tacz::nbt {
namespace {
namespace probe=pack::probe;
std::mutex mutex;
probe::DescriptorBridge bridge;
probe::Samples samples;
std::atomic<uint64_t> active=0,errors=0;
// Integrated server and client registries may have different Item allocations.
std::array<std::atomic<uintptr_t>,8> ammoItems{};
bool knownItem(uintptr_t item) {
    for (auto const& slot:ammoItems) if (item && slot.load(std::memory_order_relaxed)==item) return true;
    return false;
}
bool rememberItem(uintptr_t item) { // mutex held, only after live full-stack validation
    if (!item || knownItem(item)) return false;
    for (auto& slot:ammoItems) if (!slot.load()) { slot=item; return true; }
    return false; // bounded; extra registries fail closed, never inspect strangers
}
uint64_t generation=0,lastSummary=0,serverSerial=0;
uint64_t produced=0,ctorCalls=0,cloneCalls=0,assignCalls=0;
uint64_t lastProduced=0,lastPublished=0;
uint64_t lastRegistration=0;
uintptr_t imageBase=0,imageEnd=0;
thread_local bool inspecting=false;
struct Inspect { bool previous=inspecting; Inspect(){inspecting=true;} ~Inspect(){inspecting=previous;} };
std::vector<ll::event::ListenerPtr> listeners;
bool getHook=false,itemHook=false,componentHook=false,ctorHook=false,nameCtorHook=false,cloneHook=false,assignHook=false;
uint64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t session() {
    auto token=active.load(std::memory_order_relaxed);
#ifdef TACZ_CLIENT
    if (token!=client::shooter::worldToken()) return 0;
#endif
    return token;
}
uintptr_t callerAddress(uintptr_t address) { return address>=imageBase && address<imageEnd ? address-imageBase : address; }
bool inImage(uintptr_t address) { return address>=imageBase && address<imageEnd; }
auto& logger() { return TaczMod::getInstance().getSelf().getLogger(); }
template<class Map> std::string histogram(Map const& map) {
    std::string out;
    for (auto const& [value,count]:map) {
        if (!out.empty()) out+=",";
        if constexpr (std::is_integral_v<std::remove_cvref_t<decltype(value)>>) out+=std::to_string(value);
        else out+=value;
        out+="="+std::to_string(count);
    }
    return out.empty() ? "none" : out;
}
void print(std::map<probe::Caller,probe::Counts> const& rows,char const* phase,uint64_t token) {
    for (auto const& [site,c]:rows) logger().info(
        "[stack-probe] {} world={} caller=0x{:x} nativeImage={} thread={} route={} calls={} resolvedCandidate={} unresolved={} different={} copyHits={} native=[{}] would=[{}] ammo=[{}] resolution=[{}] observeOnly=true",
        phase,token,site.address,site.nativeImage,site.thread,site.route==0 ? "Item" : "ComponentItem",c.calls,c.resolved,c.calls-c.resolved,c.different,c.copyHits,
        histogram(c.native),histogram(c.would),histogram(c.ammo),histogram(c.status));
}
void close() {
    active=0;
    std::map<probe::Caller,probe::Counts> rows;
    uint64_t token=0,p=0,c=0,cl=0,a=0,inv=0,cap=0,drop=0,err=0;
    size_t size=0;
    {
        std::lock_guard lock(mutex);
        if (!generation) return;
        token=generation; rows=std::move(samples.total); drop=samples.dropped;
        p=produced;c=ctorCalls;cl=cloneCalls;a=assignCalls;inv=bridge.invalidated;cap=bridge.capacityCleared;size=bridge.size();
        err=errors.exchange(0);
        generation=0; for (auto& item:ammoItems) item=0; lastRegistration=0;
        bridge.clear(); samples.clear(); produced=ctorCalls=cloneCalls=assignCalls=0; lastProduced=lastPublished=0;
    }
    print(rows,"total",token);
    logger().info("[stack-probe] world-exit world={} producers={} ctor={} clone={} assignment={} invalidated={} capacityCleared={} cached={} droppedCallers={} errors={} observeOnly=true",token,p,c,cl,a,inv,cap,size,drop,err);
}
void registerAmmoItem(uint64_t token);
void heartbeat() {
#ifdef TACZ_CLIENT
    uint64_t token=client::shooter::worldToken();
    if (!token) return; // explicit endWorld closes the matching session, not a possibly stale tick
#else
    uint64_t token=active.load();
    if (!token) token=++serverSerial;
#endif
    if (active.load() && active.load()!=token) return;
    std::map<probe::Caller,probe::Counts> rows;
    bool activity=false;
    uint64_t p=0,pub=0,copies=0,inv=0,cap=0;
    size_t size=0;
    auto now=nowMs();
    bool registerItem=false;
    {
        std::lock_guard lock(mutex);
#ifdef TACZ_CLIENT
        if (token!=client::shooter::worldToken()) return;
#endif
        if (generation && generation!=token) return;
        if (!generation) { generation=token; lastSummary=now; active=token; }
        if (!ammoItems[0].load() && (!lastRegistration || now-lastRegistration>=1000)) { lastRegistration=now; registerItem=true; }
        if (now-lastSummary>=1000) {
            rows=samples.takeWindow(); lastSummary=now;
            activity=!rows.empty() || produced!=lastProduced || bridge.published!=lastPublished;
            p=produced;pub=bridge.published;copies=bridge.copied;inv=bridge.invalidated;cap=bridge.capacityCleared;size=bridge.size();
            lastProduced=p;lastPublished=pub;
        }
    }
    if (registerItem) registerAmmoItem(token);
    if (activity) logger().info("[stack-probe] bridge world={} producers={} published={} copies={} invalidated={} capacityCleared={} cached={} errors={} observeOnly=true",token,p,pub,copies,inv,cap,size,errors.load());
    if (!rows.empty()) print(rows,"summary",token);
}
bool ourAmmo(Item const& item) { return item.mFullName->getString()=="tacz:ammo"; }
void registerAmmoItem(uint64_t token) {
    // MCFOLD can share the tiny getter with unrelated classes. Resolve our Item
    // through a real temporary stack before inspecting any descriptor argument
    // in that global hook. Never insert, drop or spawn this temporary stack.
    Inspect guard;
    ItemStack stack; stack.reinit("tacz:ammo",1,0);
    if (stack.isNull()) return;
    auto descriptor=stack.getDescriptor();
    auto item=descriptor.getItem();
    if (!item || !ourAmmo(*item)) return;
    {
        std::lock_guard lock(mutex);
        if (token!=generation || token!=session()) return;
        rememberItem(reinterpret_cast<uintptr_t>(item));
    }
    logger().info("[stack-probe] registered world={} ammoItem=0x{:x} source=temporary-stack observeOnly=true",token,reinterpret_cast<uintptr_t>(item));
}
void publish(ItemStackBase const& stack,ItemDescriptor const& descriptor,uint64_t token,uintptr_t producer) {
    Inspect guard;
    auto impl=reinterpret_cast<uintptr_t>(descriptor.mImpl.get());
    std::optional<probe::DescriptorRecord> record;
    uintptr_t actual=0;
    if (stack.getTypeName()=="tacz:ammo") {
        auto item=descriptor.getItem();
        if (item && ourAmmo(*item)) {
            actual=reinterpret_cast<uintptr_t>(item);
            if (auto id=readAmmoId(stack)) if (auto ammo=TaczMod::getInstance().gunPacks().ammo(*id))
                record=probe::DescriptorRecord{actual,descriptor.getAuxValue(),pack::ammoStackLimit(ammo->index.stackSize),id->toString(),nowMs(),producer,false};
        }
    }
    bool registered=false;
    {
        std::lock_guard lock(mutex);
        if (token!=generation || token!=session()) return;
        registered=rememberItem(actual);
        ++produced; bridge.erase(GetCurrentThreadId(),impl);
        if (record) bridge.put(GetCurrentThreadId(),impl,std::move(*record));
    }
    if (registered) logger().info("[stack-probe] registered world={} ammoItem=0x{:x} source=full-stack observeOnly=true",token,actual);
}
probe::Lookup lookup(uintptr_t impl,uintptr_t item,int aux,uint64_t token) {
    std::lock_guard lock(mutex);
    if (!token || token!=generation || token!=session()) return {};
    return bridge.lookup(GetCurrentThreadId(),impl,item,aux,nowMs());
}
void invalidate(ItemDescriptor const& descriptor,uint64_t token) {
    std::lock_guard lock(mutex);
    if (!token || token!=generation || token!=session()) return;
    ++ctorCalls; bridge.erase(GetCurrentThreadId(),reinterpret_cast<uintptr_t>(descriptor.mImpl.get()));
}
void transfer(uintptr_t impl,probe::Lookup hit,uint64_t token,bool assignment) {
    std::lock_guard lock(mutex);
    if (!token || token!=generation || token!=session()) return;
    if (assignment) ++assignCalls; else ++cloneCalls;
    bridge.erase(GetCurrentThreadId(),impl);
    if (hit.record) { hit.record->copied=true; bridge.put(GetCurrentThreadId(),impl,std::move(*hit.record)); }
}
void observe(Item const& item,ItemDescriptor const& descriptor,int native,uintptr_t caller,int route,uint64_t token) {
    // Do not inspect Item fields or descriptor references of other/folded callers.
    if (!knownItem(reinterpret_cast<uintptr_t>(&item))) return;
    Inspect guard;
    auto actual=descriptor.getItem();
    auto hit=lookup(reinterpret_cast<uintptr_t>(descriptor.mImpl.get()),reinterpret_cast<uintptr_t>(actual),descriptor.getAuxValue(),token);
    if (actual!=&item) hit={probe::Resolution::FingerprintMismatch,{}};
    std::lock_guard lock(mutex);
    if (token!=generation || token!=session()) return;
    samples.add({callerAddress(caller),GetCurrentThreadId(),inImage(caller),route},native,hit);
}
static_assert(sizeof(ItemDescriptor)==16 && sizeof(InternalItemDescriptor)==24);
static_assert(std::is_same_v<decltype(&ItemStackBase::getDescriptor),ItemDescriptor (ItemStackBase::*)() const>);
LL_TYPE_INSTANCE_HOOK(ProbeDescriptor,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getDescriptor,ItemDescriptor) {
    auto token=session(); auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto descriptor=origin();
    if (token && token==session() && !inspecting) try { publish(*this,descriptor,token,callerAddress(caller)); } catch (...) { ++errors; }
    return descriptor;
}
using Max=uchar (Item::*)(ItemDescriptor const&) const;
using ComponentMax=uchar (ComponentItem::*)(ItemDescriptor const&) const;
static_assert(std::is_same_v<decltype(&Item::$getMaxStackSize),Max>);
static_assert(std::is_same_v<decltype(&ComponentItem::$getMaxStackSize),ComponentMax>);
LL_TYPE_INSTANCE_HOOK(ProbeItemLimit,ll::memory::HookPriority::Normal,Item,&Item::$getMaxStackSize,uchar,ItemDescriptor const& descriptor) {
    auto token=session(); auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto native=origin(descriptor);
    if (token && token==session() && !inspecting) try { observe(*this,descriptor,native,caller,0,token); } catch (...) { ++errors; }
    return native; // OBSERVE ONLY: never return the bridge's would-limit
}
LL_TYPE_INSTANCE_HOOK(ProbeComponentLimit,ll::memory::HookPriority::Normal,ComponentItem,&ComponentItem::$getMaxStackSize,uchar,ItemDescriptor const& descriptor) {
    auto token=session(); auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto native=origin(descriptor);
    if (token && token==session() && !inspecting) try { observe(*this,descriptor,native,caller,1,token); } catch (...) { ++errors; }
    return native;
}
using ItemCtor=void* (ItemDescriptor::*)(Item const&,int);
using NameCtor=void* (ItemDescriptor::*)(std::string_view,int);
auto const itemCtor=static_cast<ItemCtor>(&ItemDescriptor::$ctor);
auto const nameCtor=static_cast<NameCtor>(&ItemDescriptor::$ctor);
static_assert(std::is_same_v<decltype(itemCtor),ItemCtor const> && std::is_same_v<decltype(nameCtor),NameCtor const>);
LL_TYPE_INSTANCE_HOOK(ProbeItemDescriptorCtor,ll::memory::HookPriority::Normal,ItemDescriptor,itemCtor,void*,Item const& item,int aux) {
    auto token=session(); auto result=origin(item,aux);
    if (token && !inspecting) try { invalidate(*this,token); } catch (...) { ++errors; }
    return result;
}
LL_TYPE_INSTANCE_HOOK(ProbeNameDescriptorCtor,ll::memory::HookPriority::Normal,ItemDescriptor,nameCtor,void*,std::string_view name,int aux) {
    auto token=session(); auto result=origin(name,aux);
    if (token && !inspecting) try { invalidate(*this,token); } catch (...) { ++errors; }
    return result;
}
using Clone=std::unique_ptr<ItemDescriptor::BaseDescriptor> (InternalItemDescriptor::*)() const;
static_assert(std::is_same_v<decltype(&InternalItemDescriptor::$clone),Clone>);
LL_TYPE_INSTANCE_HOOK(ProbeInternalClone,ll::memory::HookPriority::Normal,InternalItemDescriptor,&InternalItemDescriptor::$clone,std::unique_ptr<ItemDescriptor::BaseDescriptor>) {
    auto token=session(); probe::Lookup hit;
    if (token && !inspecting) try { hit=lookup(reinterpret_cast<uintptr_t>(static_cast<ItemDescriptor::BaseDescriptor const*>(this)),reinterpret_cast<uintptr_t>(mItemEntry->mItem),mItemEntry->mAuxValue,token); } catch (...) { ++errors; }
    auto result=origin();
    if (token && !inspecting) try { transfer(reinterpret_cast<uintptr_t>(result.get()),std::move(hit),token,false); } catch (...) { ++errors; }
    return result;
}
using Assign=void (ItemDescriptor::*)(ItemDescriptor const&);
auto const assign=static_cast<Assign>(&ItemDescriptor::operator=);
static_assert(std::is_same_v<decltype(assign),Assign const>);
LL_TYPE_INSTANCE_HOOK(ProbeDescriptorAssign,ll::memory::HookPriority::Normal,ItemDescriptor,assign,void,ItemDescriptor const& rhs) {
    auto token=session(); probe::Lookup hit;
    if (token && !inspecting) try { hit=lookup(reinterpret_cast<uintptr_t>(rhs.mImpl.get()),reinterpret_cast<uintptr_t>(rhs.getItem()),rhs.getAuxValue(),token); } catch (...) { ++errors; }
    origin(rhs);
    if (token && !inspecting) try { transfer(reinterpret_cast<uintptr_t>(mImpl.get()),std::move(hit),token,true); } catch (...) { ++errors; }
}
}
void releaseStackLimitProbe() { try { close(); } catch (...) { active=0; ++errors; } }
void enableStackLimitProbe() {
    imageBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER const*>(imageBase);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64 const*>(imageBase+dos->e_lfanew);
    imageEnd=imageBase+nt->OptionalHeader.SizeOfImage;
    getHook=ProbeDescriptor::hook()==0; itemHook=ProbeItemLimit::hook()==0;
    auto itemAddress=ll::memory::resolveIdentifier(&Item::$getMaxStackSize);
    auto componentAddress=ll::memory::resolveIdentifier(&ComponentItem::$getMaxStackSize);
    bool folded=itemAddress==componentAddress;
    if (!folded) componentHook=ProbeComponentLimit::hook()==0;
    ctorHook=ProbeItemDescriptorCtor::hook()==0; nameCtorHook=ProbeNameDescriptorCtor::hook()==0;
    cloneHook=ProbeInternalClone::hook()==0; assignHook=ProbeDescriptorAssign::hook()==0;
    logger().info("[stack-probe] install getDescriptor={} Item={} ComponentItem={} folded={} itemCtor={} nameCtor={} clone={} assignment={} mode=observe candidateOnly=true ttlMs=250 capacity=4096 imageBase=0x{:x}",getHook,itemHook,componentHook,folded,ctorHook,nameCtorHook,cloneHook,assignHook,imageBase);
    auto& bus=ll::event::EventBus::getInstance();
    listeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([](auto&){try { heartbeat(); } catch (...) { ++errors; }}));
#ifdef TACZ_CLIENT
    listeners.push_back(bus.emplaceListener<ll::event::ClientLevelTickEvent>([](auto&){try { heartbeat(); } catch (...) { ++errors; }}));
#else
    listeners.push_back(bus.emplaceListener<ll::event::ServerStoppingEvent>([](auto&){releaseStackLimitProbe();}));
#endif
}
void disableStackLimitProbe() {
    releaseStackLimitProbe();
    auto& bus=ll::event::EventBus::getInstance();
    for (auto& listener:listeners) bus.removeListener(listener);
    listeners.clear();
    if (getHook) ProbeDescriptor::unhook(); if (itemHook) ProbeItemLimit::unhook();
    if (componentHook) ProbeComponentLimit::unhook(); if (ctorHook) ProbeItemDescriptorCtor::unhook();
    if (nameCtorHook) ProbeNameDescriptorCtor::unhook(); if (cloneHook) ProbeInternalClone::unhook();
    if (assignHook) ProbeDescriptorAssign::unhook();
    getHook=itemHook=componentHook=ctorHook=nameCtorHook=cloneHook=assignHook=false;
}
}
