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
constexpr mce::Color White{1.f,1.f,1.f,1.f},Red{1.f,1.f/3,1.f/3,1.f},Gray{2.f/3,2.f/3,2.f/3,1.f};
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
std::atomic<bool> warned{false};
std::atomic<bool> enabled{false};
model::hud::HitMarker marker;
struct Hit { uint64_t world; int64_t at; pack::ResourceId gun; bool head,kill; };
std::vector<Hit> pending;
ll::event::ListenerPtr listener;

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
    images.clear(); textureOwner.reset(); resourceKey.reset();
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
    image.mAlphaUsage=mce::AlphaUsage::Transparent;
    image.setRawImage(mce::Blob(pixels,size_t(w)*h*4));
    auto name="tacz/ui/"+std::to_string(world)+"/"+std::to_string(++serial);
    result.location=std::make_unique<ResourceLocation>(Core::PathView(name),ResourceFileSystem::Raw);
    std::shared_ptr<mce::TextureGroup> group=context.mTextureGroup;
    auto& uploaded=group->uploadTexture(*result.location,cg::ImageBuffer(std::move(image)));
    result.pointer.emplace(uploaded,*result.location,cg::TextureSetLayerType::Color);
    result.failed=false;
    return &result;
}
void image(MinecraftUIRenderContext& context,std::string const& path,glm::vec2 pos,glm::vec2 size,
    mce::Color color=White,float alpha=1,glm::vec2 uv={0,0},glm::vec2 span={1,1}) {
    auto icon=texture(context,path); if (!icon || !icon->pointer) return;
    std::shared_ptr<BedrockTextureData const> data=icon->pointer->mClientTexture;
    if (!data) return;
    context.drawImage(*data->mClientTexture,pos,size,uv,span,false);
    context.flushImages(color,alpha,HashedString{"ui_textured_and_glcolor"});
}
void text(MinecraftUIRenderContext& context,Font& font,std::string value,float x,float y,float scale,mce::Color color) {
    TextMeasureData const measure{scale,0,false,false,false,ui::TextAlignment::Left};
    CaretMeasureData const caret{-1,false};
    context.drawText(font,RectangleArea{x,x+180,y,y+16*scale},std::move(value),color,1,ui::TextAlignment::Left,measure,caret);
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
void draw(MinecraftUIRenderContext& context) {
    auto generation=shooter::worldToken(); if (!generation) return; // before ALL native queries
    IClientInstance& client=context.mClient;
    if (!shooter::ownsClient(&client)) return;
    auto player=client.getLocalPlayer(); if (!player || !player->isAlive() || player->isSpectator()) return;
    if (!client.getScreenName().starts_with("hud_screen")) return;
    std::shared_ptr<mce::TextureGroup> group=context.mTextureGroup; if (!group) return;
    ScreenContext& screenContext=context.mScreenContext;
    auto buffers=screenContext.tessellator.mBufferResourceService.lock();
    auto* builder=renderDragonFrameBuilder();
    int lighting=builder && builder->isRayTracingEnabled() ? 1 : builder && builder->isDeferredEnabled() ? 2 : 0;
    model::RenderResourceKey key{generation,reinterpret_cast<uintptr_t>(&client),reinterpret_cast<uintptr_t>(group.get()),
        reinterpret_cast<uintptr_t>(buffers.get()),reinterpret_cast<uintptr_t>(builder),lighting};
    std::lock_guard lock(cacheMutex);
    if (!enabled || generation!=shooter::worldToken()) return;
    if (!key.ready()) { releaseTextures(); return; }
    if (!resourceKey || *resourceKey!=key) {
        releaseTextures();
        if (world!=generation) marker={};
        world=generation; clientOwner=key.client; resourceKey=key; textureOwner=group;
        countAt=-1; ammoGun.clear(); warned=false;
        logger().info("[hud] bind world={} client={} textureGroup={} lighting={}",world,clientOwner,key.textures,lighting);
    }
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
    auto& font=client.getMinecraftGame_DEPRECATED().getFontRepository()->getFontFromFontType("default").getFont();
    if (ammoGun!=gun->gunId.toString() || countAt<0 || now-countAt>=50) {
        ammoGun=gun->gunId.toString(); countAt=now;
        reserve=player->isCreative() ? model::hud::MaxAmmo : data.ammoId ? nbt::countAmmo(*player,*data.ammoId) : 0;
    }
    bool inventory=data.reload.type==pack::FeedType::Inventory;
    int amount=model::hud::currentAmmo(inventory ? reserve : gun->ammo,gun->barrel,data.bolt);
    auto current=model::hud::ammoText(amount,data.ammoAmount,display->ammoPercent);
    auto spare=model::hud::reserveText(reserve,player->isCreative(),data.reload.infinite,inventory);
    auto ink=model::hud::lowAmmo(amount,data.ammoAmount) ? Red : inventory ? mce::Color{1.f,1.f,1.f/3,1.f} : White;
    context.fillRectangle(RectangleArea{width-75,width-74,height-43,height-25},White,1);
    context.flushImages(White,1,HashedString{"ui_fillColor"});
    text(context,font,current,width-70,height-43,1.5f,ink);
    float currentWidth=static_cast<float>(context.getLineLength(font,current,1,false))*1.5f;
    text(context,font,spare,width-68+currentWidth,height-43,.8f,Gray);
    context.flushText(0,std::nullopt);
    auto icon=amount<=0 && display->hudEmpty ? display->hudEmpty : display->hud;
    if (icon) image(context,pack::assetPath(*icon,"textures",".png"),{width-117,height-44},{39,13},amount<=0 && !display->hudEmpty ? mce::Color{1.f,.3f,.3f,1.f} : White);
    auto mode=gun->fireMode==pack::FireMode::Auto ? "auto" : gun->fireMode==pack::FireMode::Burst ? "burst" : "semi";
    image(context,std::string("assets/tacz/textures/hud/fire_mode_")+mode+".png",{width-68.5f+currentWidth,height-38},{10,10});
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
    std::lock_guard queueLock(queueMutex); pending.clear();
}
void disable() {
    enabled=false;
    if (listener) ll::event::EventBus::getInstance().removeListener(listener);
    listener=nullptr;
    std::lock_guard lock(cacheMutex);
    releaseTextures(); world=0; clientOwner=0; countAt=-1; marker={};
    std::lock_guard queueLock(queueMutex); pending.clear();
}
} // namespace tacz::client::hud
