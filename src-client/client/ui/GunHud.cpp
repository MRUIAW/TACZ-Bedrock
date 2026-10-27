#include "client/ui/GunHud.h"
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>
#include <stb_image.h>
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/Font.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/controls/VisualTree.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/common/Globals.h"
#include "mc/common/SharedConstants.h"
#include "mc/deps/minecraft_renderer/framebuilder/FrameBuilder.h"
#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/image/ImageFormat.h"
#include "mc/deps/core/image/ImageUsage.h"
#include "mc/deps/core/math/AlphaUsage.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/resource/ResourceFileSystem.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/world/item/ItemStack.h"
#include "client/animation/GunAnimation.h"
#include "client/gameplay/ClientAim.h"
#include "client/gameplay/ClientShooter.h"
#include "client/sound/GunSound.h"
#include "model/GunHud.h"
#include "model/RenderResourceKey.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "nbt/PackItem.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/gui/controls/renderers/InventoryItemRenderer.h"
#include "mc/client/gui/controls/renderers/HoverTextRenderer.h"
#include "mc/client/gui/controls/renderers/FlyingItem.h"
#include "mc/client/gui/controls/renderers/FlyingItemRenderer.h"
#include "mc/world/containers/SlotData.h"
#include "mc/client/gui/screens/UIScene.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "pack/ItemTooltip.h"
#include "mc/client/gui/controls/UIControl.h"
#include "model/GunHudLayout.h"
#include "model/ItemIcon.h"
#include "debug/RenderDebug.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "net/HitFeedbackPacket.h"

namespace tacz::client::hud {
namespace {
using Clock=std::chrono::steady_clock;
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
auto& logger() { return TaczMod::getInstance().getSelf().getLogger(); }
using DrawImage=void (MinecraftUIRenderContext::*)(mce::ClientTexture const&,glm::vec2 const&,glm::vec2 const&,glm::vec2 const&,glm::vec2 const&,bool);
using FlushImages=void (MinecraftUIRenderContext::*)(mce::Color const&,float,HashedString const&);
using Fill=void (MinecraftUIRenderContext::*)(RectangleArea const&,mce::Color const&,float);
using DrawText=void (MinecraftUIRenderContext::*)(Font&,RectangleArea const&,std::string&&,mce::Color const&,float,ui::TextAlignment,TextMeasureData const&,CaretMeasureData const&);
static_assert(std::is_same_v<decltype(&MinecraftUIRenderContext::drawImage),DrawImage>);
static_assert(std::is_same_v<decltype(&MinecraftUIRenderContext::flushImages),FlushImages>);
static_assert(std::is_same_v<decltype(&MinecraftUIRenderContext::fillRectangle),Fill>);
static_assert(std::is_same_v<decltype(&MinecraftUIRenderContext::drawText),DrawText>);
static_assert(std::is_same_v<decltype(&MinecraftUIRenderContext::getLineLength),int (MinecraftUIRenderContext::*)(Font&,std::string const&,float,bool) const>);
using GuiDataGetter=Bedrock::NotNullNonOwnerPtr<GuiData> (IClientInstance::*)();
static_assert(std::is_same_v<decltype(static_cast<GuiDataGetter>(&IClientInstance::getGuiData)),GuiDataGetter>);
static_assert(std::is_same_v<decltype(&IOptionRegistry::getHideHud),bool (IOptionRegistry::*)() const>);
static_assert(std::is_same_v<decltype(&SharedConstants::MajorVersion),int const& (*)()>);
static_assert(std::is_same_v<decltype(&SharedConstants::MinorVersion),int const& (*)()>);
static_assert(std::is_same_v<decltype(&SharedConstants::PatchVersion),int const& (*)()>);
static_assert(std::is_same_v<decltype(static_cast<float (Font::*)() const>(&Font::getScaleFactor)),float (Font::*)() const>);
static_assert(std::is_same_v<decltype(&Font::getWrapHeight),float (Font::*)() const>);
using FontSources=std::pair<Core::PathBuffer<std::string> const&,Core::PathBuffer<std::string> const&> (Font::*)() const;
static_assert(std::is_same_v<decltype(&Font::getFontSources),FontSources>);
constexpr mce::Color White{1.f,1.f,1.f,1.f},Gray{2.f/3,2.f/3,2.f/3,1.f};
struct Texture {
    bool failed=false;
    std::unique_ptr<ResourceLocation> location;
    std::optional<mce::TexturePtr> pointer;
};
auto& images=*new std::map<std::string,Texture>; // explicit release; no native destructor after engine shutdown
std::mutex cacheMutex,queueMutex;
uint64_t world=0,serial=0;
uintptr_t clientOwner=0;
std::optional<model::RenderResourceKey> resourceKey;
std::weak_ptr<mce::TextureGroup> textureOwner;
std::string crosshair="dot_1",ammoGun;
int64_t countAt=-1;
int reserve=0;
uint64_t loggedHudRevision=~uint64_t(0);
std::atomic<bool> warned{false};
std::atomic<bool> enabled{false};
model::hud::HitMarker marker;
struct Hit { uint64_t world; int64_t at; pack::ResourceId gun; bool head,kill; };
std::vector<Hit> pending;
ll::event::ListenerPtr listener;
std::set<std::string> iconScreens;
std::set<std::string> iconDiagnostics;
// No controller/ItemStack/native ownership retained. The renderer address is
// only a lookup key; every update replaces it, render validates owner + world.
std::map<InventoryItemRenderer const*,pack::SlotIconBinding> slotIdentities;
std::map<std::string,uint64_t> iconDrawCounts;
std::map<std::string,std::string> tooltipIcons;
uint64_t tooltipWorld=0;
thread_local bool tooltipRendering=false;
struct TooltipImage { float x,y; std::string path; };
thread_local std::optional<TooltipImage> tooltipImage;

class HitHandler final : public ll::network::PacketHandlerBase<HitHandler,net::HitFeedbackPacket> {
public:
    void handlePacket(NetworkIdentifier const&,NetEventCallback&,net::HitFeedbackPacket const& packet) const {
        auto generation=shooter::worldToken(); if (!enabled || !generation) return;
        auto gun=pack::ResourceId::parse(packet.gunId); if (!gun) return;
        {
            std::lock_guard lock(queueMutex);
            if (generation!=shooter::worldToken() || pending.size()>=128) return;
            pending.push_back({generation,nowMs(),*gun,packet.headshot,packet.kill});
        }
        ll::thread::ClientThreadExecutor::getDefault().execute([generation,gun=*gun,head=packet.headshot,kill=packet.kill] {
            if (enabled && generation==shooter::worldToken()) sound::playGunSound(gun,kill ? "kill" : head ? "head_hit" : "flesh_hit",1,1);
        });
    }
};

void releaseTextures() {
    auto owner=textureOwner.lock();
    for (auto& [id,image]:images) {
        image.pointer.reset();
        if (owner && image.location) owner->unloadTexture(*image.location,false);
    }
    images.clear(); textureOwner.reset(); resourceKey.reset(); iconScreens.clear(); iconDiagnostics.clear();
}
Texture* texture(MinecraftUIRenderContext& context,std::string const& path) {
    auto [it,inserted]=images.try_emplace(path); auto& result=it->second;
    if (!inserted) return result.failed ? nullptr : &result;
    result.failed=true;
    auto bytes=TaczMod::getInstance().gunPacks().readFile(path);
    if (!bytes) { logger().warn("[hud] missing image {}; skipped",path); return nullptr; }
    if (bytes->size()>static_cast<size_t>(std::numeric_limits<int>::max())) return nullptr;
    int w=0,h=0,n=0;
    auto pixels=stbi_load_from_memory(reinterpret_cast<stbi_uc const*>(bytes->data()),static_cast<int>(bytes->size()),&w,&h,&n,4);
    if (!pixels) { logger().warn("[hud] invalid PNG {}; skipped",path); return nullptr; }
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> owned(pixels,stbi_image_free);
    if (w<1 || h<1 || w>8192 || h>8192) return nullptr;
    mce::Image image(static_cast<uint32>(w),static_cast<uint32>(h),mce::ImageFormat::RGBA8Unorm,mce::ImageUsage::SRGB);
    image.setRawImage(mce::Blob(pixels,size_t(w)*h*4));
    image.mAlphaUsage=mce::AlphaUsage::Transparent; // set AFTER raw-image replacement
    auto alpha=model::iconAlpha({pixels,size_t(w)*h*4});
    auto name="tacz/ui/"+std::to_string(world)+"/"+std::to_string(++serial);
    result.location=std::make_unique<ResourceLocation>(Core::PathView(name),ResourceFileSystem::Raw);
    std::shared_ptr<mce::TextureGroup> group=context.mTextureGroup;
    auto& uploaded=group->uploadTexture(*result.location,cg::ImageBuffer(std::move(image)));
    result.pointer.emplace(uploaded,*result.location,cg::TextureSetLayerType::Color);
    result.failed=false;
    logger().info("[ui-texture] uploaded path={} size={}x{} world={} RGBA8=true alphaZero={} alphaPartial={} alphaOpaque={}",path,w,h,world,alpha.transparent,alpha.partial,alpha.opaque);
    return &result;
}
bool image(MinecraftUIRenderContext& context,std::string const& path,glm::vec2 pos,glm::vec2 size,
    mce::Color color=White,float alpha=1,glm::vec2 uv={0,0},glm::vec2 span={1,1},bool sprite=false) {
    auto icon=texture(context,path); if (!icon || !icon->pointer) return false;
    std::shared_ptr<BedrockTextureData const> data=icon->pointer->mClientTexture;
    if (!data) return false;
    context.drawImage(*data->mClientTexture,pos,size,uv,span,false);
    context.flushImages(color,alpha,HashedString{sprite ? model::SlotIconMaterial : "ui_textured_and_glcolor"});
    return true;
}
void text(MinecraftUIRenderContext& context,Font& font,std::string value,glm::vec2 pos,float scale,mce::Color color,bool shadow=false) {
    TextMeasureData const measure{scale,0,shadow,false,false,ui::TextAlignment::Left};
    CaretMeasureData const caret{-1,false};
    context.drawText(font,RectangleArea{pos.x,pos.x+180,pos.y,pos.y+16*scale},std::move(value),color,1,ui::TextAlignment::Left,measure,caret);
}
std::shared_ptr<Font> bitmapFont(FontRepository const& repo) {
    auto const& originals=repo.mOriginalMinecraftFonts.get();
    if (auto it=originals.find(repo.mDefaultFontId);it!=originals.end() && it->second) return it->second;
    // A handle retains its original/default font even when getFont() resolves a
    // modern UI font override. Do not alter repository-wide font preferences.
    return repo.mFontHandle->mDefaultFont;
}
mce::Color rgb(uint32_t value) {
    return {float((value>>16)&255)/255.f,float((value>>8)&255)/255.f,float(value&255)/255.f,1.f};
}
void configure() {
    try {
        auto path=TaczMod::getInstance().getSelf().getModDir()/"config"/"hud.json";
        nlohmann::json config=nlohmann::json::object();
        if (std::ifstream input{path};input) config=nlohmann::json::parse(input);
        crosshair=model::hud::crosshairType(config.value("crosshair_type",std::string("dot_1")));
        config["crosshair_type"]=crosshair;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out{path}; out<<config.dump(2)<<'\n';
    } catch (std::exception const& e) { logger().warn("[hud] config unavailable: {}; using dot_1",e.what()); crosshair="dot_1"; }
}
bool bindImages(MinecraftUIRenderContext& context) { // cacheMutex held by caller
    auto generation=shooter::worldToken(); if (!generation || !enabled) return false;
    IClientInstance& client=context.mClient;
    if (!shooter::ownsClient(&client)) return false;
    std::shared_ptr<mce::TextureGroup> group=context.mTextureGroup; if (!group) return false;
    ScreenContext& screenContext=context.mScreenContext;
    auto buffers=screenContext.tessellator.mBufferResourceService.lock();
    auto* builder=renderDragonFrameBuilder();
    int lighting=builder && builder->isRayTracingEnabled() ? 1 : builder && builder->isDeferredEnabled() ? 2 : 0;
    model::RenderResourceKey key{generation,reinterpret_cast<uintptr_t>(&client),reinterpret_cast<uintptr_t>(group.get()),
        reinterpret_cast<uintptr_t>(buffers.get()),reinterpret_cast<uintptr_t>(builder),lighting};
    if (!enabled || generation!=shooter::worldToken()) return false;
    if (!key.ready()) { releaseTextures(); return false; }
    if (!resourceKey || *resourceKey!=key) {
        releaseTextures();
        if (world!=generation) marker={};
        world=generation; clientOwner=key.client; resourceKey=key; textureOwner=group;
        countAt=-1; ammoGun.clear(); warned=false;
        logger().info("[hud] bind world={} client={} textureGroup={} lighting={}",world,clientOwner,key.textures,lighting);
    }
    return true;
}
using SlotRender=void (InventoryItemRenderer::*)(MinecraftUIRenderContext&,IClientInstance&,UIControl&,int);
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$render),SlotRender>);
using SlotUpdate=bool (InventoryItemRenderer::*)(IClientInstance&,UIControl&,UIScene const&);
using CollectionVisitor=brstd::function_ref<void(std::string const&,int)>;
static_assert(sizeof(CollectionVisitor)==16 && !std::is_empty_v<CollectionVisitor>);
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$update),SlotUpdate>);
static_assert(std::is_same_v<decltype(&UIControl::findAllCollectionIndices),void (UIControl::*)(CollectionVisitor)>);
static_assert(std::is_same_v<decltype(&UIControl::findCollectionIndex),int (UIControl::*)(std::string const&) const>);
static_assert(std::is_same_v<decltype(&ContainerScreenController::_getVisualItemStack),ItemStackBase const& (ContainerScreenController::*)(std::string const&,int) const>);
static_assert(std::is_same_v<decltype(&ContainerScreenController::mCursorSlotData),SlotData const& (*)()>);
static_assert(std::is_same_v<decltype(&ContainerScreenController::_isCursorSelectedActive),bool (ContainerScreenController::*)() const>);
std::optional<pack::ItemIdentity> slotIdentity(InventoryItemRenderer const* renderer,UIControl const* owner=nullptr) {
    std::lock_guard lock(cacheMutex);
    auto it=slotIdentities.find(renderer);
    if (it==slotIdentities.end() || !it->second.matches(shooter::worldToken(),reinterpret_cast<uintptr_t>(owner),renderer->mItemInstance->getId(),renderer->mItemInstance->getAuxValue())) return std::nullopt;
    return it->second.identity;
}
bool refreshSlotIdentity(InventoryItemRenderer& renderer,IClientInstance& client,UIControl& owner,UIScene const& scene) {
    auto generation=shooter::worldToken();
    if (!enabled || !generation || !shooter::ownsClient(&client)) return false;
    pack::SlotIdentitySelection selection;
    std::string sourceCollection,sourceType;
    int sourceIndex=-1,sourceId=0;
    auto const& visual=renderer.mItemInstance.get();
    auto view=scene.mScreenView.get();
    auto controller=view ? view->mController.get() : nullptr;
    auto containers=controller && controller->_isContainerScreen() ? static_cast<ContainerScreenController*>(controller) : nullptr;
    // The renderer's control can be below the grid item carrying collection
    // indices. Only walk ancestors, never children/siblings or a hovered slot.
    bool diagnose=false;
    if (!visual.isNull()) {
        std::lock_guard lock(cacheMutex);
        auto prefix="ancestor-sample:"+client.getScreenName()+":"+(visual.getTypeName().starts_with("tacz:") ? "pack:" : "any:");
        for (int n=0;n<3;++n) if (iconDiagnostics.insert(prefix+std::to_string(n)).second) { diagnose=true; break; }
    }
    std::vector<std::vector<pack::SlotCollectionIndex>> levels;
    std::vector<UIControl const*> seen;
    std::shared_ptr<UIControl> ancestor;
    auto current=&owner;
    bool cursorIcon=false;
    for (int depth=0;current && depth<32;++depth) {
        if (std::find(seen.begin(),seen.end(),current)!=seen.end()) break;
        seen.push_back(current);
        cursorIcon=cursorIcon || model::cursorIconControl(current->mName.get());
        std::vector<pack::SlotCollectionIndex> indices;
        // The SDK function_ref callback's int is pointer-like in 26.51. Never
        // use it as an index. String names match the native UI bindings; validate
        // their contents and query the plain, separately checked scalar getter.
        auto collect=[&](std::string const& collection,int) {
            if (!pack::validSlotCollectionIndex(collection,0)) return;
            int index=current->findCollectionIndex(collection);
            if (std::none_of(indices.begin(),indices.end(),[&](auto const& slot){return slot.collection==collection;}))
                indices.push_back({collection,index});
            std::lock_guard lock(cacheMutex);
            auto key="slot-index:"+client.getScreenName()+":"+collection+":"+std::to_string(index);
            if (iconDiagnostics.size()<4096 && iconDiagnostics.insert(key).second)
                logger().info("[item-icons] slot-index screen={} depth={} control={} collection={}[{}] via=findCollectionIndex nameValid=true inRange={}",
                    client.getScreenName(),depth,current->mName.get(),collection,index,pack::validSlotCollectionIndex(collection,index));
        };
        current->findAllCollectionIndices(collect);
        levels.push_back(std::move(indices));
        if (diagnose) {
            logger().info("[item-icons] ancestor screen={} depth={} control={} collectionCount={}",client.getScreenName(),depth,current->mName.get(),levels.back().size());
            for (auto const& slot:levels.back()) logger().info("[item-icons] ancestor-index screen={} depth={} control={} collection={}[{}]",client.getScreenName(),depth,current->mName.get(),slot.collection,slot.index);
        }
        if (!diagnose && pack::nearestSlotCollectionLevel(levels)) break;
        ancestor=current->mParent.lock();
        current=ancestor.get();
    }
    auto sourceDepth=pack::nearestSlotCollectionLevel(levels);
    auto visit=[&](std::string const& collection,int index) {
        if (!pack::validSlotCollectionIndex(collection,index)) return;
        ItemStackBase const* full=nullptr;
        if (containers) full=&containers->_getVisualItemStack(collection,index);
        else if (client.getScreenName().starts_with("hud_screen") && collection=="hotbar_items" && index<9) {
            if (auto player=client.getLocalPlayer()) full=&player->getInventory().getItem(index);
        }
        if (!full || full->isNull()) return;
        auto fullIdentity=nbt::readItemIdentity(*full);
        bool matched=selection.offer(visual.getId(),visual.getAuxValue(),full->getId(),full->getAuxValue(),fullIdentity);
        if (fullIdentity || visual.getTypeName().starts_with("tacz:")) {
            std::lock_guard lock(cacheMutex);
            auto key="resolve:"+client.getScreenName()+":"+collection+":"+std::to_string(index)+":"+std::to_string(full->getId());
            if (iconDiagnostics.insert(key).second) logger().info(
                "[item-icons] resolve screen={} depth={} collection={}[{}] renderType={} copyId={} copyAux={} copyNBT={} sourceType={} sourceId={} sourceAux={} sourceNBT={} identity={} matched={}",
                client.getScreenName(),sourceDepth.value_or(0),collection,index,visual.getTypeName(),visual.getId(),visual.getAuxValue(),bool(visual.mUserData),
                full->getTypeName(),full->getId(),full->getAuxValue(),bool(full->mUserData),fullIdentity ? fullIdentity->id.toString() : "none",matched);
        }
        if (!matched) return;
        sourceCollection=collection; sourceIndex=index; sourceType=full->getTypeName(); sourceId=full->getId();
    };
    // This is the fully defined SDK function_ref, NOT an opaque placeholder.
    // Nearest collection scope wins, even if unresolved: do not silently fall
    // through to an unrelated outer grid when this slot is empty/ambiguous.
    if (cursorIcon && containers) {
        if (containers->_isCursorSelectedActive()) {
            auto const& slot=ContainerScreenController::mCursorSlotData();
            visit(slot.mCollectionName.get(),slot.mCollectionIndex);
            if (diagnose) logger().info("[item-icons] cursor-source screen={} collection={}[{}] identity={}",client.getScreenName(),slot.mCollectionName.get(),slot.mCollectionIndex,selection.identity ? selection.identity->id.toString() : "none");
        }
    } else if (sourceDepth) for (auto const& slot:levels[*sourceDepth]) visit(slot.collection,slot.index);
    if (diagnose) logger().info("[item-icons] ancestor-result screen={} sourceDepth={} identity={} ambiguous={}",client.getScreenName(),sourceDepth ? int(*sourceDepth) : -1,selection.identity ? selection.identity->id.toString() : "none",selection.ambiguous);
    auto identity=selection.identity; // ambiguity fails closed; never use a neighbour's gun
    auto previous=slotIdentity(&renderer,&owner);
    {
        std::lock_guard lock(cacheMutex);
        if (slotIdentities.size()>8192) slotIdentities.clear();
        slotIdentities[&renderer]={generation,reinterpret_cast<uintptr_t>(&owner),visual.getId(),visual.getAuxValue(),identity};
        auto key="source:"+client.getScreenName()+":"+sourceCollection+":"+std::to_string(sourceIndex)+":"+(identity ? identity->id.toString() : "none");
        if (identity && iconDiagnostics.insert(key).second)
            logger().info("[item-icons] source screen={} collection={}[{}] type={} runtimeId={} renderItemId={} copyId={} copyNBT={} copyIdentity={} fullIdentity={}",
                client.getScreenName(),sourceCollection,sourceIndex,sourceType,sourceId,renderer.mItemId,visual.getId(),bool(visual.mUserData),
                nbt::readItemIdentity(visual).has_value(),identity->id.toString());
    }
    return previous!=identity;
}
bool slotHasIcon(InventoryItemRenderer const* renderer) {
    auto identity=slotIdentity(renderer);
    return identity && pack::itemIcon(TaczMod::getInstance().gunPacks(),*identity).has_value();
}
using CloneSlot=std::shared_ptr<UICustomRenderer> (InventoryItemRenderer::*)() const;
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$clone),CloneSlot>);
LL_TYPE_INSTANCE_HOOK(PackSlotClone,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$clone,std::shared_ptr<UICustomRenderer>) {
    auto result=origin();
    if (result && enabled && shooter::worldToken()) {
        // This native override clones InventoryItemRenderer, not an arbitrary
        // renderer. Copy values only; the clone must resolve its own owner at render.
        auto clone=static_cast<InventoryItemRenderer*>(result.get());
        std::lock_guard lock(cacheMutex);
        auto it=slotIdentities.find(this);
        if (it!=slotIdentities.end() && it->second.matches(shooter::worldToken(),0,mItemInstance->getId(),mItemInstance->getAuxValue())) {
            auto binding=it->second; binding.owner=0; slotIdentities[clone]=std::move(binding);
        }
    }
    return result;
}
LL_TYPE_INSTANCE_HOOK(PackSlotUpdate,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$update,bool,
    IClientInstance& client,UIControl& owner,UIScene const& scene) {
    bool changed=origin(client,owner,scene);
    bool identityChanged=refreshSlotIdentity(*this,client,owner,scene);
    // A drawImage/flushImages call is immediate, not persistent mesh content.
    // Keep these custom slots dirty so native UI caching never replays an empty mesh.
    return changed || identityChanged || (enabled && shooter::worldToken() && shooter::ownsClient(&client) && slotHasIcon(this));
}
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$getNumRenderPasses),int (InventoryItemRenderer::*)() const>);
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$getUIMaterialType),UIMaterialType (InventoryItemRenderer::*)(int) const>);
LL_TYPE_INSTANCE_HOOK(PackSlotPasses,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$getNumRenderPasses,int) {
    if (enabled && shooter::worldToken() && slotHasIcon(this)) return 1;
    return origin();
}
LL_TYPE_INSTANCE_HOOK(PackSlotMaterial,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$getUIMaterialType,UIMaterialType,int pass) {
    if (!pass && enabled && shooter::worldToken() && slotHasIcon(this)) return UIMaterialType::TexturedAndGlcolorSprite;
    return origin(pass);
}
static_assert(std::is_same_v<decltype(&InventoryItemRenderer::$getBatchType),UIBatchType (InventoryItemRenderer::*)() const>);
LL_TYPE_INSTANCE_HOOK(PackSlotBatch,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$getBatchType,UIBatchType) {
    auto native=origin();
    if (!enabled || !shooter::worldToken()) return native;
    auto identity=slotIdentity(this);
    {
        std::lock_guard lock(cacheMutex);
        auto key="batch-entry:"+std::to_string(mItemId)+":"+(identity ? identity->id.toString() : "none");
        if (iconDiagnostics.size()<128 && iconDiagnostics.insert(key).second)
            logger().info("[item-icons] batch-entry itemId={} count={} identity={} native={}",mItemId,mItemInstance->mCount,identity ? identity->id.toString() : "none",static_cast<int>(native));
    }
    if (!identity || !pack::itemIcon(TaczMod::getInstance().gunPacks(),*identity)) return native;
    // Flat items normally enter an atlas SharedMesh batch. That path does not
    // call render(), and binding a standalone PNG inside it would be wrong.
    // Use the engine's uncached custom-render path only for our pack-slot items.
    std::lock_guard lock(cacheMutex);
    if (iconDiagnostics.insert("batch:"+identity->id.toString()).second)
        logger().info("[item-icons] batch id={} native={} selected=Default world={}",identity->id.toString(),static_cast<int>(native),shooter::worldToken());
    return UIBatchType::Default;
}
LL_TYPE_INSTANCE_HOOK(PackSlotIcon,ll::memory::HookPriority::Normal,InventoryItemRenderer,&InventoryItemRenderer::$render,void,
    MinecraftUIRenderContext& context,IClientInstance& client,UIControl& owner,int pass) {
    // This common slot renderer serves hotbar/inventory/container slots. No
    // by-value UIItemRenderInfo, RenderMetadata or function_ref placeholder hooks.
    if (pass>0 && enabled && shooter::worldToken() && shooter::ownsClient(&client) && !mHidingIcon) {
        auto identity=slotIdentity(this,&owner);
        if (identity && pack::itemIcon(TaczMod::getInstance().gunPacks(),*identity)) return; // flat PNG has no native mesh/glint passes
    }
    if (enabled && shooter::worldToken() && shooter::ownsClient(&client)) {
        std::lock_guard lock(cacheMutex);
        auto key="render-entry:"+client.getScreenName()+":"+std::to_string(pass)+":"+std::to_string(mHidingIcon);
        if (iconDiagnostics.size()<128 && iconDiagnostics.insert(key).second)
            logger().info("[item-icons] render-entry screen={} pass={} hidden={}",client.getScreenName(),pass,mHidingIcon);
    }
    if (enabled && !pass && shooter::worldToken() && shooter::ownsClient(&client) && !mHidingIcon) {
        try {
            auto identity=slotIdentity(this,&owner);
            if (!identity) {
                refreshSlotIdentity(*this,client,owner,context.mCurrentScene);
                identity=slotIdentity(this,&owner);
            }
            auto path=identity ? pack::itemIcon(TaczMod::getInstance().gunPacks(),*identity) : std::nullopt;
            {
                std::lock_guard lock(cacheMutex);
                auto key="entry:"+client.getScreenName()+":"+(identity ? identity->id.toString() : "none");
                if (iconDiagnostics.size()<128 && iconDiagnostics.insert(key).second)
                    logger().info("[item-icons] entry screen={} id={} slot={}",client.getScreenName(),identity ? identity->id.toString() : "none",path.value_or("missing"));
            }
            if (path) {
                std::lock_guard lock(cacheMutex);
                if (bindImages(context)) {
                    owner._setCachedPosition();
                    auto pos=owner.mCachedPosition.get(),size=owner.mSize.get();
                    if (size.x>0 && size.y>0 && image(context,*path,pos,size,White,owner.mAlpha,{0,0},{1,1},true)) {
                        if (iconScreens.insert(client.getScreenName()+":"+*path).second) logger().info("[item-icons] rendered screen={} path={} pos=({}, {}) size=({}, {})",
                            client.getScreenName(),*path,pos.x,pos.y,size.x,size.y);
                        auto count=++iconDrawCounts[client.getScreenName()+":"+*path];
                        if (count==1 || count==120 || count==1200) logger().info("[item-icons] redraw screen={} path={} draws={} material={}",client.getScreenName(),*path,count,model::SlotIconMaterial);
                        return;
                    }
                }
                if (iconDiagnostics.insert("failed:"+client.getScreenName()+":"+*path).second)
                    logger().warn("[item-icons] paint failed screen={} path={}; native fallback",client.getScreenName(),*path);
            }
        } catch (std::exception const& e) { if (!warned.exchange(true)) logger().warn("[item-icons] {}; using native placeholder",e.what()); }
    }
    origin(context,client,owner,pass);
}
using FlyingDraw=void (*)(MinecraftUIRenderContext&,IClientInstance&,ui::FlyingItem&,float,float,float,float,int);
static_assert(std::is_same_v<decltype(&FlyingItemRenderer::_renderGuiIcon),FlyingDraw>);
LL_TYPE_STATIC_HOOK(PackFlyingIcon,ll::memory::HookPriority::Normal,FlyingItemRenderer,&FlyingItemRenderer::_renderGuiIcon,void,
    MinecraftUIRenderContext& context,IClientInstance& client,ui::FlyingItem& item,float x,float y,float alpha,float scale,int zOrder) {
    if (enabled && shooter::worldToken() && shooter::ownsClient(&client) && std::isfinite(scale) && scale>0) try {
        auto identity=nbt::readItemIdentity(item.item.get());
        auto path=identity ? pack::itemIcon(TaczMod::getInstance().gunPacks(),*identity) : std::nullopt;
        if (path) {
            std::lock_guard lock(cacheMutex);
            if (bindImages(context) && image(context,*path,{x,y},{16*scale,16*scale},White,alpha,{0,0},{1,1},true)) {
                if (iconDiagnostics.insert("flying:"+client.getScreenName()+":"+*path).second)
                    logger().info("[item-icons] flying rendered screen={} identity={} x={} y={} scale={} zOrder={}",client.getScreenName(),identity->id.toString(),x,y,scale,zOrder);
                return;
            }
        }
    } catch (std::exception const& e) { if (!warned.exchange(true)) logger().warn("[item-icons] flying fallback: {}",e.what()); }
    origin(context,client,item,x,y,alpha,scale,zOrder);
}
using TooltipFontSignature=void (Font::*)(ScreenContext&,std::string_view,float,float,mce::Color const&,bool,bool,bool,mce::MaterialPtr const*,int,bool,float,mce::Color const&,mce::Color const&,float,float,OffscreenCaptureDescription const&,bool);
static_assert(std::is_same_v<decltype(&Font::$drawCached),TooltipFontSignature>);
static_assert(std::is_same_v<decltype(&HoverTextRenderer::$render),void (HoverTextRenderer::*)(MinecraftUIRenderContext&,IClientInstance&,UIControl&,int)>);
LL_TYPE_INSTANCE_HOOK(PackTooltipFont,ll::memory::HookPriority::Normal,Font,&Font::$drawCached,void,
    ScreenContext& screen,std::string_view str,float x,float y,mce::Color const& color,bool ignore,bool darken,bool symbol,
    mce::MaterialPtr const* material,int caret,bool shadow,float padding,mce::Color const& reset,mce::Color const& dark,
    float outline,float caretY,OffscreenCaptureDescription const& capture,bool normals) {
    if (tooltipRendering && !tooltipImage && shooter::worldToken()) {
        std::lock_guard lock(cacheMutex);
        if (tooltipWorld==shooter::worldToken()) for (auto const& [text,path]:tooltipIcons) {
            auto line=pack::tooltipAmmoIconLine(str,text); if (!line) continue;
            tooltipImage=TooltipImage{x,y+static_cast<float>(*line)*(getWrapHeight()+padding),path};
            if (iconDiagnostics.insert("tooltip-spacing").second)
                logger().info("[item-tooltip] native wrapHeight={} padding={} (unchanged: background measurement must match text)",getWrapHeight(),padding);
            break;
        }
    }
    origin(screen,str,x,y,color,ignore,darken,symbol,material,caret,shadow,padding,reset,dark,outline,caretY,capture,normals);
}
LL_TYPE_INSTANCE_HOOK(PackTooltipPaint,ll::memory::HookPriority::Normal,HoverTextRenderer,&HoverTextRenderer::$render,void,
    MinecraftUIRenderContext& context,IClientInstance& client,UIControl& owner,int pass) {
    if (!enabled || !shooter::worldToken() || !shooter::ownsClient(&client)) { origin(context,client,owner,pass); return; }
    tooltipImage.reset();
    struct Scope { Scope(){tooltipRendering=true;} ~Scope(){tooltipRendering=false;} } scope;
    origin(context,client,owner,pass);
    if (!tooltipImage) return;
    auto icon=std::move(*tooltipImage); tooltipImage.reset();
    context.flushText(0,std::nullopt);
    std::lock_guard lock(cacheMutex);
    if (bindImages(context) && image(context,icon.path,{icon.x,icon.y},{16,16})) {
        if (iconDiagnostics.insert("tooltip:"+client.getScreenName()).second)
            logger().info("[item-icons] tooltip painted screen={} path={}",client.getScreenName(),icon.path);
    }
}
bool slotHook=false,slotUpdateHook=false,batchHook=false,tooltipFontHook=false,tooltipPaintHook=false,passesHook=false,materialHook=false,flyingHook=false,cloneHook=false;
void draw(MinecraftUIRenderContext& context) {
    auto generation=shooter::worldToken(); if (!generation) return;
    IClientInstance& client=context.mClient;
    if (!shooter::ownsClient(&client)) return;
    auto player=client.getLocalPlayer(); if (!player || !player->isAlive() || player->isSpectator()) return;
    if (!client.getScreenName().starts_with("hud_screen")) return;
    std::lock_guard lock(cacheMutex);
    if (!bindImages(context)) return;
    auto now=nowMs();
    std::vector<Hit> hits;
    { std::lock_guard queueLock(queueMutex); hits.swap(pending); }
    for (auto const& hit:hits) if (hit.world==world && now-hit.at<=500) {
        marker.mark(hit.at,hit.kill,hit.head);
    }
    if (client.getOptions().getHideHud()) return;
    auto gun=nbt::readGun(player->getSelectedItem()); if (!gun) return;
    auto const& packs=TaczMod::getInstance().gunPacks();
    auto entry=packs.gun(gun->gunId);
    auto display=entry && entry->index.display ? packs.gunDisplay(*entry->index.display) : nullptr;
    if (!entry || !display) return;
    auto const& data=entry->data;
    auto const& screen=client.getGuiData()->mScreenSizeData->clientUIScreenSize.get();
    float width=screen.x,height=screen.y;
    if (!std::isfinite(width) || !std::isfinite(height) || width<130 || height<60) return;
    auto repo=client.getMinecraftGame_DEPRECATED().getFontRepository();
    auto bitmap=bitmapFont(*repo);
    if (!bitmap) return; // repository not yet ready; no dangling/cached font reference
    auto& font=*bitmap;
    float fontUnit=model::hud::bitmapScale(static_cast<float>(context.getLineLength(font,"0",1,false)));
    auto const& selected=player->getSelectedItem();
    auto const& tag=selected.mUserData;
    bool dummy=tag && tag->contains("DummyAmmo",Tag::Int);
    if (ammoGun!=gun->gunId.toString() || countAt<0 || now-countAt>=50) {
        ammoGun=gun->gunId.toString(); countAt=now;
        reserve=player->isCreative() ? model::hud::MaxAmmo : dummy ? std::max(0,static_cast<int>((*tag)["DummyAmmo"]))
            : data.ammoId ? nbt::countAmmo(*player,*data.ammoId) : 0;
    }
    bool inventory=data.reload.type==pack::FeedType::Inventory;
    int amount=model::hud::currentAmmo(inventory ? reserve : gun->ammo,gun->barrel,data.bolt);
    auto current=model::hud::ammoText(amount,data.ammoAmount,display->ammoPercent);
    auto spare=model::hud::reserveText(reserve,player->isCreative(),data.reload.infinite,inventory);
    float currentWidth=static_cast<float>(context.getLineLength(font,current,fontUnit,false));
    auto hud=model::hud::layout(width,height,currentWidth);
    context.fillRectangle(RectangleArea{hud.line.x,hud.line.z,hud.line.y,hud.line.w},White,1);
    context.flushImages(White,1,HashedString{"ui_fillColor"});
    text(context,font,current,hud.current,fontUnit*model::hud::Layout::CurrentScale,rgb(model::hud::currentColor(amount,data.ammoAmount,inventory,dummy)));
    text(context,font,spare,hud.reserve,fontUnit*model::hud::Layout::ReserveScale,rgb(model::hud::reserveColor(inventory,dummy)));
    auto gameVersion=std::to_string(SharedConstants::MajorVersion())+"."+std::to_string(SharedConstants::MinorVersion())
        +"."+std::to_string(SharedConstants::PatchVersion());
    auto version=model::hud::versionText(gameVersion,TACZ_VERSION);
    text(context,font,version,hud.version,fontUnit*model::hud::Layout::VersionScale,Gray,true);
    context.flushText(0,std::nullopt);
    auto icon=amount<=0 && display->hudEmpty ? display->hudEmpty : display->hud;
    if (icon) image(context,pack::assetPath(*icon,"textures",".png"),hud.gunIcon,model::hud::Layout::GunSize,amount<=0 && !display->hudEmpty ? mce::Color{1.f,.3f,.3f,1.f} : White);
    auto mode=gun->fireMode==pack::FireMode::Auto ? "auto" : gun->fireMode==pack::FireMode::Burst ? "burst" : "semi";
    image(context,std::string("assets/tacz/textures/hud/fire_mode_")+mode+".png",hud.fireMode,model::hud::Layout::ModeSize);
    auto hudRevision=debug::hudRevision.load();
    if (loggedHudRevision!=hudRevision) {
        loggedHudRevision=hudRevision;
        auto sources=font.getFontSources();
        logger().info("[hud-layout] world={} gun={} magazine={} rawChamber={} bolt={} effective={} text={} reserve={} percent={} GUI={}x{} guiScale={} bitmap={} digitAdvance={} fontUnit={} wrapHeight={} nativeScale={} version={}",
            world,gun->gunId.toString(),gun->ammo,gun->barrel,data.bolt ? static_cast<int>(*data.bolt) : -1,amount,current,spare,display->ammoPercent,
            width,height,client.getGuiData()->mGuiScale,static_cast<std::string const&>(sources.first),
            context.getLineLength(font,"0",1,false),fontUnit,font.getWrapHeight(),font.getScaleFactor(),version);
    }
    auto animation=anim::hudState(); auto aiming=aim::snapshot();
    bool same=animation.world==world && animation.gun==gun->gunId && animation.client==clientOwner;
    model::hud::CrosshairState cross;
    cross.holding=true; cross.firstPerson=client.getOptions().getPlayerViewPerspective()==0;
    cross.reloading=gun::isReloading(shooter::reloadState()); cross.animationHide=same && animation.hideCrosshair;
    cross.puttingAway=animation.puttingAway; cross.forceInAds=display->showCrosshair;
    cross.aim=aiming.world==world && aiming.gun==gun->gunId ? aiming.progress : 0;
    if (model::hud::crosshairVisible(cross)) image(context,"assets/tacz/textures/crosshair/normal/"+crosshair+".png",{width/2-8,height/2-8},{16,16},White,.9f);
    auto hit=marker.sample(now);
    if (hit.alpha>0) {
        auto color=hit.red ? mce::Color{1.f,0.f,0.f,1.f} : White;
        for (int q=0;q<4;++q) {
            float u=q%2 ? .5f : 0,v=q/2 ? .5f : 0;
            float x=width/2-8+(q%2 ? 8+hit.offset : -hit.offset);
            float y=height/2-8+(q/2 ? 8+hit.offset : -hit.offset);
            image(context,"assets/tacz/textures/crosshair/hit/hit_marker.png",{x,y},{8,8},color,hit.alpha,{u,v},{.5f,.5f});
        }
    }
}
} // namespace
void enable() {
    configure();
    enabled=true;
    slotHook=PackSlotIcon::hook()==0;
    slotUpdateHook=PackSlotUpdate::hook()==0;
    batchHook=PackSlotBatch::hook()==0;
    passesHook=PackSlotPasses::hook()==0;
    materialHook=PackSlotMaterial::hook()==0;
    flyingHook=PackFlyingIcon::hook()==0;
    cloneHook=PackSlotClone::hook()==0;
    tooltipFontHook=PackTooltipFont::hook()==0;
    tooltipPaintHook=PackTooltipPaint::hook()==0;
    logger().info("[item-icons] update={} render={} batchRoute={} tooltipFont={} tooltipPaint={} (actual screen draws logged separately)",slotUpdateHook,slotHook,batchHook,tooltipFontHook,tooltipPaintHook);
    logger().info("[item-icons] flatPasses={} spriteMaterial={} flying={} clone={} cursor=controller-slot",passesHook,materialHook,flyingHook,cloneHook);
    listener=ll::event::EventBus::getInstance().emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& event) {
        if (!shooter::worldToken()) return;
        auto const& tree=event.screenView().mVisualTree;
        if (!tree || !std::string_view(*tree->mRootControlName).ends_with(".hud_screen")) return;
        try { draw(event.uiRenderContext()); }
        catch (std::exception const& e) { if (!warned.exchange(true)) logger().warn("[hud] draw failed: {}",e.what()); }
    });
}
void releaseWorld(IClientInstance const* client) {
    std::lock_guard lock(cacheMutex);
    if (clientOwner && clientOwner!=reinterpret_cast<uintptr_t>(client)) return;
    releaseTextures(); world=0; clientOwner=0; countAt=-1; ammoGun.clear(); marker={};
    tooltipIcons.clear(); tooltipWorld=0; tooltipImage.reset();
    slotIdentities.clear();
    iconDrawCounts.clear();
    std::lock_guard queueLock(queueMutex); pending.clear();
}
void rememberTooltip(std::string text,std::string icon) {
    auto generation=shooter::worldToken(); if (!enabled || !generation) return;
    std::lock_guard lock(cacheMutex);
    if (tooltipWorld!=generation) tooltipIcons.clear();
    tooltipWorld=generation;
    if (tooltipIcons.size()>=16) tooltipIcons.clear();
    tooltipIcons[std::move(text)]=std::move(icon);
}
void disable() {
    enabled=false;
    if (slotHook) PackSlotIcon::unhook(); slotHook=false;
    if (slotUpdateHook) PackSlotUpdate::unhook(); slotUpdateHook=false;
    if (batchHook) PackSlotBatch::unhook(); batchHook=false;
    if (passesHook) PackSlotPasses::unhook(); passesHook=false;
    if (materialHook) PackSlotMaterial::unhook(); materialHook=false;
    if (flyingHook) PackFlyingIcon::unhook(); flyingHook=false;
    if (cloneHook) PackSlotClone::unhook(); cloneHook=false;
    if (tooltipFontHook) PackTooltipFont::unhook(); tooltipFontHook=false;
    if (tooltipPaintHook) PackTooltipPaint::unhook(); tooltipPaintHook=false;
    if (listener) ll::event::EventBus::getInstance().removeListener(listener);
    listener=nullptr;
    std::lock_guard lock(cacheMutex);
    releaseTextures(); world=0; clientOwner=0; countAt=-1; marker={};
    tooltipIcons.clear(); tooltipWorld=0; tooltipImage.reset();
    slotIdentities.clear();
    iconDrawCounts.clear();
    std::lock_guard queueLock(queueMutex); pending.clear();
}
} // namespace tacz::client::hud
