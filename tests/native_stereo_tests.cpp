#include "stereo_stream.hpp"
#include "../src/game32/render_trace.hpp"
#include "../src/game32/native_stereo.hpp"
#include "../src/game32/stereo_request_gate.hpp"
#include "../src/host64/include/kotorvr/host/stereo_resolution.hpp"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <string_view>
#include <string>

int main(int argc,char** argv) {
    using namespace k2vr;
    int failures=0;
    const auto check=[&](bool ok,const char* name) { if (!ok) { ++failures; std::cerr<<name<<'\n'; } };
    const auto close=[](float a,float b) { return std::abs(a-b)<0.0001F; };
    if (argc==3 && std::string_view(argv[1])=="--check-eye-percent") {
        const auto expected=static_cast<std::uint32_t>(std::strtoul(argv[2],nullptr,10));
        check(_putenv_s("KOTOR2VR_EYE_RESOLUTION","")==0,"isolate percentage environment test");
        const auto& setting=kotorvr::host::NativeStereoEyeResolutionSetting();
        check(setting.percent==expected,"inherited eye resolution parsed or falls back to 75");
        check(kotorvr::host::NativeStereoEyeWidth(2001)==ipc::StereoEyeDimension(2001,expected),
            "host dimensions use the inherited resolution");
        check(_putenv_s("KOTOR2VR_EYE_PERCENT",expected==50 ? "100":"50")==0,"change test process environment");
        check(kotorvr::host::NativeStereoEyeResolutionSetting().percent==expected &&
            kotorvr::host::NativeStereoEyeWidth(2001)==ipc::StereoEyeDimension(2001,expected),
            "eye resolution remains stable after process environment changes");
        std::cout<<"Eye resolution environment "<<(failures ? "FAIL":"PASS")<<" percent="<<expected<<'\n';
        return failures ? 1:0;
    }
    int controlled_object{},nearby_object{};
    check(game32::ShouldHideFirstPersonObjectDraw(true,0,&controlled_object,&controlled_object),
        "controlled model scope is eligible for per-mesh filtering in the first eye");
    check(!game32::ShouldHideFirstPersonObjectDraw(true,0,&controlled_object,&nearby_object),
        "nearby actors, scenery and independently rendered attachments remain visible");
    check(!game32::ShouldHideFirstPersonObjectDraw(false,0,&controlled_object,&controlled_object) &&
        !game32::ShouldHideFirstPersonObjectDraw(true,-1,&controlled_object,&controlled_object) &&
        !game32::ShouldHideFirstPersonObjectDraw(true,1,&controlled_object,&controlled_object) &&
        !game32::ShouldHideFirstPersonObjectDraw(true,0,nullptr,nullptr) &&
        !game32::ShouldHideFirstPersonObjectDraw(true,0,&controlled_object,nullptr),
        "disabled/authored/monitor passes, replays and missing object scopes never select a new hidden draw");
    int head_object{},head_hook{},hand_hook{},head_accessory{},other_head{},other_hook{};
    using HeadLink=game32::EgoHeadAttachmentLink;
    const std::array<HeadLink,1> human_head{{{&head_object,&controlled_object,&head_hook,true}}};
    check(game32::ControlledHeadAttachment(&controlled_object,&head_hook,&head_object,human_head),
        "separate human head bound to the controlled body's HeadHook is selected");
    // Original pmbam Torso/LArm/RArm use 3081/1092/1068 indices.
    // None may qualify merely because they share a skinned shader or nearby pivot.
    check(!game32::ControlledHeadAttachment(&controlled_object,&head_hook,&controlled_object,{}) &&
        !game32::ControlledHeadAttachment(&controlled_object,&head_hook,&controlled_object,human_head),
        "human body Gob including torso, arms and legs stays visible");
    const std::array<HeadLink,1> weapon{{{&nearby_object,&controlled_object,&hand_hook,true}}};
    const std::array<HeadLink,1> foreign{{{&other_head,&nearby_object,&other_hook,true}}};
    check(!game32::ControlledHeadAttachment(&controlled_object,&head_hook,&nearby_object,weapon) &&
        !game32::ControlledHeadAttachment(&controlled_object,&head_hook,&other_head,foreign),
        "hand-bound weapons and other characters' heads remain visible");
    const std::array<HeadLink,2> accessory{{
        {&head_accessory,&head_object,&other_hook,true},human_head[0]}};
    check(game32::ControlledHeadAttachment(&controlled_object,&head_hook,&head_accessory,accessory),
        "verified accessories beneath the controlled head are selected");
    for (unsigned bad=0;bad<5;++bad) {
        auto broken=human_head;
        if (bad==0) broken[0].supported=false;
        if (bad==1) broken[0].child=&other_head;
        if (bad==2) broken[0].parent=nullptr;
        if (bad==3) broken[0].node=nullptr;
        if (bad==4) broken[0].parent=&head_object;
        check(!game32::ControlledHeadAttachment(&controlled_object,&head_hook,&head_object,broken),
            "unsupported, stale, null or self-referencing attachment links remain visible");
    }
    check(!game32::ControlledHeadAttachment(&controlled_object,nullptr,&head_object,human_head),
        "missing HeadHook never falls back to body/proximity hiding");
    const std::array<HeadLink,3> cycle{{
        {&head_accessory,&head_object,&other_hook,true},
        {&head_object,&head_accessory,&other_hook,true},
        {&head_accessory,&controlled_object,&head_hook,true}}};
    check(!game32::ControlledHeadAttachment(&controlled_object,&head_hook,&head_accessory,cycle),
        "cyclic or inconsistent ancestry is rejected");
    int chain_objects[10]{};
    std::array<HeadLink,9> deep{};
    for (unsigned i=0;i<deep.size();++i) deep[i]={&chain_objects[i],
        i+1==deep.size() ? static_cast<const void*>(&controlled_object):&chain_objects[i+1],&head_hook,true};
    check(!game32::ControlledHeadAttachment(&controlled_object,&head_hook,&chain_objects[0],deep),
        "attachment traversal is capped at eight links");
    check(game32::ShortEgoModel({0,0,0},{0,0,0.8F}) &&
        game32::ShortEgoModel({100,-20,30},{100,-20,30.8F}) &&
        !game32::ShortEgoModel({0,0,0},{0,0,1.635F}) &&
        !game32::ShortEgoModel({0,0,0},{0,0,1.2F}) &&
        !game32::ShortEgoModel({0,0,0},{0,0,0}),
        "model-height dispatch covers T3 and shifted map origins, preserving normal-height selection");
    // Rounded root-space pivots from the original p_t3m4 MDL bind pose. Head
    // has 439 faces; Eyes has 46 and shares its pivot. The upper gun is a
    // separate descendant with its own pivot, not part of this selection.
    game32::EgoMeshSelection t3{};
    t3.short_model=true; t3.head_pivot_mask=3;
    const math::Vec3 t3_head{0.0019F,0.0116F,0.7119F};
    t3.head_pivots[0]=t3.head_pivots[1]=t3_head;
    check(game32::NamedEgoHeadPivot(t3,t3_head),
        "large low T3 Head and co-located Eyes qualify without shader or 768-index assumptions");
    for (const math::Vec3 part:std::array<math::Vec3,5>{{
        {0.0019F,-0.3586F,0.1200F}, // chassis
        {-0.2661F,0.0100F,0.5075F}, // left arm/leg
        {-0.2621F,0.4100F,0.0795F}, // front foot
        {0.0019F,0.0079F,0.5684F}, // neck
        {0.0257F,-0.0460F,0.8174F}, // upper gun arm
    }}) check(!game32::NamedEgoHeadPivot(t3,part),
        "original T3 chassis/leg/foot/neck/top-gun pivots remain visible");
    check(game32::NamedEgoHeadPivot(t3,t3_head+math::Vec3{0.019F,0,0}) &&
        !game32::NamedEgoHeadPivot(t3,t3_head+math::Vec3{0.021F,0,0}),
        "head-pivot tolerance is limited to 0.02 engine units");
    const auto animated_head=t3_head+math::Vec3{0.12F,-0.07F,0.04F};
    t3.head_pivots[0]=t3.head_pivots[1]=animated_head;
    check(game32::NamedEgoHeadPivot(t3,animated_head) &&
        !game32::NamedEgoHeadPivot(t3,t3_head),
        "refreshing named pivots follows animation and rejects a stale bind-pose position");
    t3.head_pivot_mask=0;
    check(!game32::NamedEgoHeadPivot(t3,animated_head),"missing named head hooks retain visibility");
    t3.head_pivot_mask=1; t3.head_pivots[0].z=std::numeric_limits<float>::quiet_NaN();
    check(!game32::NamedEgoHeadPivot(t3,animated_head),
        "nonfinite pivots and model transforms are never hidden");
    t3.head_pivots[0]={0,0,1.7F}; t3.short_model=false;
    check(!game32::NamedEgoHeadPivot(t3,t3.head_pivots[0]),
        "tall integrated models do not receive short-model pivot filtering");
    // Initialize the cached snapshot with fixed eyes, then mutate both environment inputs.
    check(_putenv_s("KOTOR2VR_EYE_RESOLUTION","2040x2232")==0,"set fixed test environment");
    check(_putenv_s("KOTOR2VR_EYE_PERCENT","bad")==0,"fixed mode overrides invalid percent");
    const auto& cached=kotorvr::host::NativeStereoEyeResolutionSetting();
    check(cached.valid && cached.fixed && cached.width==2040 && cached.height==2232,
        "fixed environment snapshot parsed");
    check(_putenv_s("KOTOR2VR_EYE_RESOLUTION","1024x1024")==0 &&
        _putenv_s("KOTOR2VR_EYE_PERCENT","50")==0,"mutate resolution environment");
    for (const auto recommendation:{1000U,3000U}) {
        check(kotorvr::host::NativeStereoEyeWidth(recommendation)==2040 &&
            kotorvr::host::NativeStereoEyeHeight(recommendation)==2232,
            "cached fixed width and height ignore runtime recommendations and environment changes");
    }
    const ipc::SessionNonce nonce{GetTickCount64(),static_cast<std::uint64_t>(GetCurrentProcessId())};
    ipc::RenderRequest request{};
    request.header=ipc::MakeHeader<ipc::RenderRequest>(ipc::MessageType::RenderRequest,1,nonce,1);
    request.frame_id=42; request.predicted_display_time_ns=1;
    request.render_width=1500; request.render_height=1600;
    request.views[0]={{-0.032F,1.6F,0,0,0,0,1},{-0.9F,0.7F,0.8F,-0.8F}};
    request.views[1]={{ 0.032F,1.6F,0,0,0,0,1},{-0.7F,0.9F,0.8F,-0.8F}};
    {
        game32::StereoRequestGate gate;
        game32::StereoCaptureKey key{};
        key.request=request; key.request.presentation_state=ipc::PresentationState::WorldFirstPerson;
        key.camera=0x1234;
        check(!gate.CanReuseView(key,100),"unpublished/failed first pair remains renderable");
        gate.Published(key,100,false);
        check(gate.CanReuseView(key,119),"same published request/eyes can be sampled once per XR request");
        check(!gate.CanReuseView(key,120) && !gate.CanReuseView(key,99),"stalled request and backwards clock never hold an old image");
        auto next=key; ++next.request.frame_id;
        check(!gate.CanReuseView(next,101) && !gate.CanReuseView(next,102),"unpublished new request remains retryable");
        check(gate.CanReuseView(key,102),"admission checks do not consume or replace published history");
        for (unsigned change=0;change<10;++change) {
            next=key;
            switch(change) {
            case 0: ++next.request.header.generation; break;
            case 1: ++next.request.header.session_nonce.high; break;
            case 2: ++next.request.predicted_display_time_ns; break;
            case 3: ++next.request.render_width; break;
            case 4: next.request.views[0].fov.angle_left+=0.1F; break;
            case 5: next.request.views[1].pose.position_y+=0.1F; break;
            case 6: ++next.camera; break;
            case 7: next.eyes[0].position_x+=0.01F; break;
            case 8: next.eyes[1].orientation_y+=0.01F; break;
            case 9: next.request.presentation_state=ipc::PresentationState::WorldThirdPerson; break;
            }
            check(!gate.CanReuseView(next,101),"new runtime, view, anchor, camera or mode is never skipped");
        }
        next=key; next.request.history_reset_reasons=static_cast<std::uint32_t>(ipc::ResetReason::Recenter);
        gate.Published(next,100,false);
        check(!gate.CanReuseView(next,101),"reset frames render even if their request repeats");
        gate.Published(key,100,true);
        check(!gate.CanReuseView(key,101),"HUD-detected dialogue bypasses request sampling");
        gate.Published(key,100,false); gate.Reset();
        check(!gate.CanReuseView(key,101),"context recovery or leaving VR clears sampling history");
    }
    {
        game32::StereoRequestGate gate;
        game32::StereoCaptureKey key{};
        key.request=request; key.request.presentation_state=ipc::PresentationState::WorldFirstPerson;
        key.camera=0x4321;
        check(!gate.CanReuseView(key,100,true),"cadence never skips an unpublished first pair");
        gate.Published(key,100,false);
        auto moving=key;
        moving.eyes[0].position_x+=2.F; moving.eyes[1].position_x+=2.F;
        moving.eyes[1].orientation_y=0.25F;
        check(!gate.CanReuseView(moving,101) && gate.CanReuseView(moving,101,true),
            "only cadence ignores engine eye-anchor motion for the same full XR request");
        check(!gate.CanReuseView(key,120) && gate.CanReuseView(moving,199,true),
            "legacy 20ms limit stays unchanged; cadence survives an ASW24 interval");
        check(!gate.CanReuseView(moving,200,true) && !gate.CanReuseView(moving,99,true),
            "cadence refreshes at 100ms exactly and rejects backwards ticks");
        // Every request byte remains part of the key, including host timing,
        // pose/FOV, generation/nonce, dimensions, reset flags and sequence.
        for (std::size_t byte=0;byte<sizeof(key.request);++byte) {
            auto changed=key;
            reinterpret_cast<unsigned char*>(&changed.request)[byte]^=1;
            check(!gate.CanReuseView(changed,101,true),"cadence must compare the FULL host request");
        }
        auto changed=key; ++changed.camera;
        check(!gate.CanReuseView(changed,101,true),"cadence respects engine camera identity");
        changed=key; changed.camera=0; gate.Published(changed,100,false);
        check(!gate.CanReuseView(changed,101,true),"cadence rejects a null camera even after publication");
        for (unsigned reset=0;reset<=10;++reset) {
            changed=key; changed.request.history_reset_reasons=1U<<reset;
            gate.Published(changed,100,false);
            check(!gate.CanReuseView(changed,101,true),"published reset requests never qualify for cadence reuse");
        }
        for (unsigned mode=0;mode<=static_cast<unsigned>(ipc::PresentationState::LoadingTheater);++mode) {
            changed=key; changed.request.presentation_state=static_cast<ipc::PresentationState>(mode);
            gate.Published(changed,100,false);
            const bool world=changed.request.presentation_state==ipc::PresentationState::WorldFirstPerson ||
                changed.request.presentation_state==ipc::PresentationState::WorldThirdPerson;
            check(gate.CanReuseView(changed,101,true)==world,"cadence is world-only, never dialogue/UI/theater/loading");
        }
        gate.Published(key,100,true);
        check(!gate.CanReuseView(moving,101,true),"HUD-discovered dialogue also bypasses cadence");
        gate.Published(key,100,false);
        changed=key; ++changed.request.frame_id;
        check(!gate.CanReuseView(changed,101,true) && !gate.CanReuseView(changed,110,true),
            "failed/unpublished new pair stays retryable and is not committed by admission");
        check(gate.CanReuseView(moving,199,true) && !gate.CanReuseView(moving,200,true),
            "failed attempts and reuse checks cannot advance the publication watchdog");
        gate.Published(changed,200,false);
        check(!gate.CanReuseView(key,201,true) && gate.CanReuseView(changed,299,true) &&
            !gate.CanReuseView(changed,300,true),"only Published changes the cadence key and clock");
        gate.Reset();
        check(!gate.CanReuseView(changed,201,true),"context/VR reset invalidates cadence history");
    }
    if (argc==4) {
        const ipc::SessionNonce shared_nonce{std::strtoull(argv[2],nullptr,10),std::strtoull(argv[3],nullptr,10)};
        const bool publish=std::string_view(argv[1])=="--publish-shared";
        const auto ack_nonce=ipc::StereoStreamNonce(shared_nonce);
        const auto ack_name=ipc::MakeGpuStreamObjectName(ack_nonce,ipc::GpuStreamObjectKind::ConsumedFence);
        HANDLE ack=CreateEventW(nullptr,TRUE,FALSE,ack_name.c_str());
        if (!ack) return 2;
        ipc::StereoFrameMapping map;
        if (publish) {
            request.header.session_nonce=shared_nonce;
            ipc::StereoFrameMetadata frame{};
            frame.request=request; frame.ready_value=17; frame.camera_frame_id=987654321;
            frame.guide_mask=3; frame.depth_mask=3; frame.engine_units_per_metre=2;
            frame.depth[0]={0.25F,250.F,0.2F,0.8F}; frame.depth[1]={0.5F,400.F,0.F,1.F};
            for (auto& matrix:frame.view_projection) for (unsigned d=0;d<4;++d) matrix.value[d*5]=1;
            if (!map.Open(shared_nonce,true) || !map.Write(frame)) { CloseHandle(ack); return 3; }
            map.MarkWorldRendered();
            const bool read=WaitForSingleObject(ack,10000)==WAIT_OBJECT_0;
            CloseHandle(ack);
            return read ? 0:4;
        }
        bool ok=false;
        for (unsigned retry=0;retry<200;++retry) {
            ipc::StereoFrameMetadata frame{};
            if (map.Open(shared_nonce,false) && map.Read(17,frame)) {
                ok=frame.camera_frame_id==987654321 && frame.request.frame_id==42 &&
                    close(frame.request.views[0].pose.position_x,-0.032F) &&
                    close(frame.request.views[1].pose.position_x,0.032F) && map.WorldRecentlyRendered() &&
                    frame.structure_size==384 && frame.depth_mask==3 && frame.engine_units_per_metre==2 &&
                    close(frame.depth[0].near_z,0.25F) && close(frame.depth[0].min_depth,0.2F) &&
                    close(frame.depth[1].far_z,400.F);
                break;
            }
            Sleep(10);
        }
        SetEvent(ack); CloseHandle(ack);
        std::cout<<"Cross-process stereo metadata "<<(ok ? "PASS":"FAIL")<<" pointer_bits="<<sizeof(void*)*8<<'\n';
        return ok ? 0:5;
    }
    check(ipc::ValidStereoRequest(request),"valid asymmetric tracked eyes");
    auto invalid=request; invalid.views[1].pose.orientation_w=0;
    check(!ipc::ValidStereoRequest(invalid),"invalid second eye rejected atomically");
    invalid=request; invalid.views[0].fov.angle_right=std::numeric_limits<float>::quiet_NaN();
    check(!ipc::ValidStereoRequest(invalid),"NaN projection rejected");
    const float tan_y=std::tan(ipc::StereoCullingFovDegrees(request)*math::kPi/360.F);
    const float tan_x=tan_y*static_cast<float>(request.render_width)/static_cast<float>(request.render_height);
    for (const auto& v:request.views) check(tan_x>=-std::tan(v.fov.angle_left) &&
        tan_x>=std::tan(v.fov.angle_right) && tan_y>=std::tan(v.fov.angle_up) &&
        tan_y>=-std::tan(v.fov.angle_down),"culling frustum encloses both asymmetric projections");
    static_assert(ipc::StereoEyeDimension(2001)==1501);
    static_assert(ipc::StereoEyeDimension(2000)==1500);
    static_assert(ipc::StereoEyeDimension(2001,50)==1001);
    static_assert(ipc::StereoEyeDimension(2001,85)==1701);
    static_assert(ipc::StereoEyeDimension(2001,100)==2001);
    static_assert(ipc::StereoEyeDimension(0,75)==0);
    static_assert(ipc::StereoEyeDimension(2001,49)==0);
    static_assert(ipc::StereoEyeDimension(2001,101)==0);
    static_assert(ipc::StereoEyeDimension(UINT32_MAX,100)==UINT32_MAX);
    static_assert(ipc::StereoEyeDimension(UINT32_MAX,75)==3221225472U);
    static_assert(ipc::ParseStereoEyePercent("50")==50);
    static_assert(ipc::ParseStereoEyePercent("75")==75);
    static_assert(ipc::ParseStereoEyePercent("85")==85);
    static_assert(ipc::ParseStereoEyePercent("100")==100);
    for (const auto value:{"","49","101","-75","+75","75x","75.0","75%"," 75","75 ","999999999999"})
        check(!ipc::ParseStereoEyePercent(value),"malformed or out-of-range eye percentage rejected");
    const auto absent=kotorvr::host::ParseStereoEyeResolutionSetting(nullptr);
    const auto selected=kotorvr::host::ParseStereoEyeResolutionSetting("85");
    const auto rejected=kotorvr::host::ParseStereoEyeResolutionSetting("bad");
    check(absent.valid && !absent.from_environment && absent.percent==75,"missing eye setting preserves default quality");
    check(selected.valid && selected.from_environment && selected.percent==85,"valid eye setting honored");
    check(!rejected.valid && rejected.from_environment && rejected.percent==75,"bad eye setting preserves default and reports invalid");
    const auto fixed=kotorvr::host::ParseStereoEyeResolutionSetting("bad","2040x2232");
    check(fixed.valid && fixed.fixed && fixed.from_environment &&
        fixed.width==2040 && fixed.height==2232,"fixed dimensions override percentage");
    for (const auto recommendation:{1000U,3000U}) {
        check(kotorvr::host::StereoEyeWidth(recommendation,fixed)==2040 &&
            kotorvr::host::StereoEyeHeight(recommendation,fixed)==2232,
            "two runtime recommendations produce identical asymmetric fixed eyes");
        check(kotorvr::host::StereoEyeWidth(recommendation,selected)==ipc::StereoEyeDimension(recommendation,85) &&
            kotorvr::host::StereoEyeHeight(recommendation+100,selected)==ipc::StereoEyeDimension(recommendation+100,85),
            "percentage width and height scale independently");
    }
    for (const auto value:{"256x256","4096x3328"})
        check(kotorvr::host::ParseStereoEyeResolutionSetting(nullptr,value).valid,
            "fixed safe boundaries accepted");
    for (const auto value:{"","2040","2040X2232","2040*2232","2040x","x2232",
        "2040x2232x256"," 2040x2232","2040x2232 ","+2040x2232","-2040x2232",
        "2040.0x2232","2040x2232junk","2041x2232","2040x2233","254x256",
        "256x254","4098x256","256x4098","256x3330","4096x4096",
        "999999999999999999999x2232","2040x999999999999999999999"}) {
        const auto invalid=kotorvr::host::ParseStereoEyeResolutionSetting("85",value);
        check(!invalid.valid && invalid.fixed && invalid.from_environment &&
            kotorvr::host::StereoEyeWidth(2000,invalid)==0 &&
            kotorvr::host::StereoEyeHeight(3000,invalid)==0,
            "invalid explicit fixed setting rejected without percentage fallback");
    }
    const auto baseline=game32::StereoHeadCenter(request);
    auto tilted=baseline;
    const auto tilted_q=math::Multiply(math::FromAxisAngle({0,1,0},0.7F),
        math::Multiply(math::FromAxisAngle({1,0,0},-0.45F),math::FromAxisAngle({0,0,1},0.3F)));
    tilted.orientation_x=tilted_q.x; tilted.orientation_y=tilted_q.y;
    tilted.orientation_z=tilted_q.z; tilted.orientation_w=tilted_q.w;
    tilted.position_y=1.4F;
    const auto upright_baseline=game32::UprightRecenterPose(tilted);
    const auto baseline_up=math::Rotate(game32::OpenXrQuaternion(upright_baseline),{0,1,0});
    check(close(baseline_up.x,0) && close(baseline_up.y,1) && close(baseline_up.z,0) &&
        close(upright_baseline.position_y,1.4F),"recenter retains gravity and seated position despite head pitch/roll");
    check(close(upright_baseline.orientation_y,std::sin(0.35F)),"recenter preserves yaw heading");
    auto vertical=baseline; vertical.orientation_x=std::sqrt(0.5F); vertical.orientation_w=std::sqrt(0.5F);
    const auto stable_vertical=game32::UprightRecenterPose(vertical,upright_baseline);
    check(close(stable_vertical.orientation_y,upright_baseline.orientation_y),"vertical gaze keeps previous yaw without a heading jump");
    game32::EngineCameraPoseWxyz authored{};
    authored.position_x=10; authored.position_y=20; authored.position_z=30;
    // Authored camera maps camera-local Y-up to the engine's world Z-up.
    authored.orientation_w=std::sqrt(0.5F); authored.orientation_x=std::sqrt(0.5F);
    // Synthetic model-hook fixtures: verify character-dependent height without
    // claiming a measured in-game height for these models.
    const math::Vec3 feet{10,20,30}, human_eye{10,20,31.65F}, droid_eye{10,20,30.8F};
    const auto human=game32::SelectFirstPersonEye(feet,true,human_eye,true,droid_eye);
    const auto droid=game32::SelectFirstPersonEye(feet,true,droid_eye,false,{});
    check(human.hook==game32::FirstPersonHook::FreeLook && close(human.world.z,31.65F),
        "FreeLookHook takes precedence over CameraHook");
    const auto first=game32::ComposeFirstPersonAnchor(authored,human.world,1,0.10F);
    const auto short_first=game32::ComposeFirstPersonAnchor(authored,droid.world,1,0.10F);
    check(first.valid && short_first.valid && close(first.pose.position_x,10) &&
        close(first.pose.position_y,20.10F) && close(first.pose.position_z,31.65F) &&
        close(short_first.pose.position_z,30.8F),"each actor retains its own model eye height");
    const auto secondary=game32::SelectFirstPersonEye(feet,false,{},true,human_eye);
    check(secondary.hook==game32::FirstPersonHook::Camera,
        "CameraHook is used when FreeLookHook is absent");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    const auto invalid_primary=game32::SelectFirstPersonEye(feet,true,{nan,20,31},true,droid_eye);
    check(invalid_primary.hook==game32::FirstPersonHook::Camera && close(invalid_primary.world.z,30.8F),
        "invalid primary hook falls back to the current actor's CameraHook");
    const auto missing=game32::SelectFirstPersonEye(feet,false,human_eye,false,droid_eye);
    check(missing.hook==game32::FirstPersonHook::None && close(missing.world.z,0),
        "missing hooks after actor switch cannot retain the previous actor eye");
    check(game32::SelectFirstPersonEye(feet,true,{10,20,300},true,{20,20,31}).hook==game32::FirstPersonHook::None &&
        game32::SelectFirstPersonEye({nan,20,30},true,human_eye,false,{}).hook==game32::FirstPersonHook::None &&
        game32::SelectFirstPersonEye(feet,true,feet,false,{}).hook==game32::FirstPersonHook::None,
        "nonfinite feet, implausible hooks and root-at-feet are rejected");
    const auto shifted=game32::SelectFirstPersonEye({-100,200,-50},true,{-100,200,-49.2F},false,{});
    check(shifted.hook==game32::FirstPersonHook::FreeLook && close(shifted.world.z,-49.2F),
        "eye validation is relative to feet and independent of map origin");
    auto pitched=authored;
    const auto pitched_q=math::Multiply(math::FromAxisAngle({0,0,1},math::kPi*0.5F),
        math::FromAxisAngle({1,0,0},math::kPi/3));
    pitched.orientation_w=pitched_q.w; pitched.orientation_x=pitched_q.x;
    pitched.orientation_y=pitched_q.y; pitched.orientation_z=pitched_q.z;
    const auto level=game32::ComposeFirstPersonAnchor(pitched,human.world,2,0.10F);
    const auto level_forward=math::Rotate(game32::EngineCameraQuaternion(level.pose),{0,0,-1});
    const auto level_up=math::Rotate(game32::EngineCameraQuaternion(level.pose),{0,1,0});
    check(level.valid && close(level_forward.x,-1) && close(level_forward.z,0) && close(level_up.z,1) &&
        close(level.pose.position_x,9.8F) && close(level.pose.position_z,31.65F),
        "heading stays upright; scale affects forward offset without scaling or double-adding eye height");
    check(!game32::ComposeFirstPersonAnchor(authored,human_eye,-1,0).valid &&
        !game32::ComposeFirstPersonAnchor(authored,{0,0,nan},1,0).valid,
        "invalid world eye or scale is rejected");
    const auto left=game32::ComposeStereoEyePose(authored,baseline,request,0);
    const auto right=game32::ComposeStereoEyePose(authored,baseline,request,1);
    check(left.valid && right.valid && close(right.pose.position_x-left.pose.position_x,0.064F),"64mm IPD survives authored rotation");
    auto yaw=baseline; yaw.orientation_y=std::sqrt(0.5F); yaw.orientation_w=std::sqrt(0.5F);
    const auto turned=game32::ComposeHmdCameraPose(authored,baseline,yaw);
    const auto forward=math::Rotate(game32::EngineCameraQuaternion(turned.pose),math::Vec3{0,0,-1});
    check(turned.valid && close(forward.x,-1) && close(forward.y,0) && close(forward.z,0),"HMD yaw turns about world up for upright authored camera");
    auto leaning=request;
    for (auto& v:leaning.views) { v.pose.position_x+=1; v.pose.position_y+=1; }
    const auto leaned_l=game32::ComposeStereoEyePose(authored,baseline,leaning,0);
    const auto leaned_r=game32::ComposeStereoEyePose(authored,baseline,leaning,1);
    check(close((leaned_l.pose.position_x+leaned_r.pose.position_x)*0.5F,10.2F) &&
        close(leaned_l.pose.position_z,30.1F) && close(leaned_r.pose.position_x-leaned_l.pose.position_x,0.064F),
        "seated lean clamp preserves independent IPD");
    const auto centered=game32::ComposeStereoEyePose(authored,game32::StereoHeadCenter(leaning),leaning,0);
    check(close(centered.pose.position_x,9.968F) && close(centered.pose.position_z,30.F),"recenter removes lean without collapsing eyes");
    // v4 GPU/mapping names are disjoint even when the session nonce repeats.
    const ipc::SessionNonce legacy_nonce{nonce.low^0x53544552454F3031ULL,nonce.high};
    check(std::wstring_view(ipc::MakeGpuStreamObjectName(legacy_nonce,ipc::GpuStreamObjectKind::Color).c_str())!=
        std::wstring_view(ipc::MakeGpuStreamObjectName(ipc::StereoStreamNonce(nonce),ipc::GpuStreamObjectKind::Color).c_str()),
        "v4 and v5 transport objects cannot mix");
    {
        auto old_name=ipc::MakeGpuStreamObjectName(legacy_nonce,ipc::GpuStreamObjectKind::Color);
        const std::wstring legacy_mapping=std::wstring(old_name.c_str())+L"-metadata";
        HANDLE old_map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,1040,legacy_mapping.c_str());
        ipc::StereoFrameMapping new_reader;
        check(old_map && !new_reader.Open(nonce,false),"v5 cannot open a live v4/336-byte mapping");
        if (old_map) CloseHandle(old_map);
    }
    ipc::StereoFrameMapping writer,reader,duplicate;
    check(writer.Open(nonce,true) && reader.Open(nonce,false),"open shared stereo metadata");
    check(!duplicate.Open(nonce,true),"refuse second metadata producer");
    ipc::StereoFrameMetadata frame{}; frame.request=request; frame.ready_value=1; frame.camera_frame_id=99;
    ipc::StereoFrameMetadata received{};
    check(!reader.Read(1,received),"unpublished pair is invisible");
    frame.eyes_complete=1;
    check(!writer.Write(frame),"never publish a left-only pair");
    frame.eyes_complete=3;
    frame.hud_width=2500; frame.hud_height=1440; frame.hud_source_aspect=3440.F/1440.F;
    check(writer.Write(frame) && reader.Read(1,received) && received.camera_frame_id==99 &&
        received.request.frame_id==42,"pair retains exact render-time pose and simulation identity");
    check(received.hud_width==2500 && received.hud_height==1440 &&
        close(received.hud_source_aspect,3440.F/1440.F),"HUD rectangle and aspect commit with the same eye pair");
    frame.hud_height=ipc::kStereoHudMaximumHeight+1;
    check(!writer.Write(frame),"HUD cannot read beyond atlas rows");
    frame.hud_height=1440; frame.hud_width=request.render_width*2+1;
    check(!writer.Write(frame),"HUD cannot read beyond atlas columns");
    frame.hud_width=2500;
    frame.guide_mask=1;
    check(!writer.Write(frame),"one eye's guides cannot be published alone");
    frame.guide_mask=3;
    check(!writer.Write(frame),"zero camera matrices rejected");
    for (unsigned i=0;i<2;++i) for (unsigned d=0;d<4;++d) frame.view_projection[i].value[d*5]=1;
    frame.view_projection[1].value[12]=0.064F;
    check(writer.Write(frame) && reader.Read(1,received) && received.guide_mask==3 &&
        close(received.view_projection[1].value[12],0.064F),"distinct guide cameras commit with eyes and HUD");
    frame.view_projection[0].value[0]=std::numeric_limits<float>::quiet_NaN();
    check(!writer.Write(frame),"invalid camera guide rejected");
    frame.view_projection[0].value[0]=1;
    check(frame.depth_mask==0 && ipc::ValidStereoDepth(frame),"depth is optional for new-format frames");
    const auto gl_projection=math::OpenGlProjection({-0.9F,0.7F,0.8F,-0.8F},0.5F,500.F);
    check(gl_projection.has_value(),"test projection exists");
    const auto metric=game32::NativeStereoDepthRange(*gl_projection,request.views[0].fov,0.5,500,2,0.2,0.8);
    check(metric && close(metric->near_z,0.25F) && close(metric->far_z,250) &&
        close(metric->min_depth,0.2F) && close(metric->max_depth,0.8F),
        "actual clip planes use the same engine-units/metre scale as the eye camera");
    const auto reversed=game32::NativeStereoDepthRange(*gl_projection,request.views[0].fov,0.5,500,2,0.8,0.2);
    check(reversed && close(reversed->near_z,250) && close(reversed->far_z,0.25F) &&
        close(reversed->min_depth,0.2F) && close(reversed->max_depth,0.8F),
        "reversed GL depth normalizes XR endpoints without changing packed samples");
    auto other_projection=*gl_projection; other_projection.value[14]*=2;
    check(!game32::NativeStereoDepthRange(other_projection,request.views[0].fov,0.5,500,2,0,1),
        "clips from a different actual projection are not advertised");
    check(!game32::NativeStereoDepthRange(*gl_projection,request.views[1].fov,0.5,500,2,0,1),
        "asymmetric FOV mismatch is not advertised");
    for (const auto scale:{0.F,-1.F,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        check(!ipc::StereoDepthRangeFromOpenGl(0.5,500,scale,0,1),"invalid metric scale rejected");
    check(!ipc::StereoDepthRangeFromOpenGl(0.5,500,1,0.5,0.5) &&
        !ipc::StereoDepthRangeFromOpenGl(0.5,500,1,-0.1,1) &&
        !ipc::StereoDepthRangeFromOpenGl(0.5,500,1,0,1.1) &&
        !ipc::StereoDepthRangeFromOpenGl(0,500,1,0,1) &&
        !ipc::StereoDepthRangeFromOpenGl(1,1,1,0,1) &&
        !ipc::StereoDepthRangeFromOpenGl(0.5,500,(std::numeric_limits<double>::min)(),0,1),
        "degenerate/out-of-range depth, clips and float overflow rejected");
    frame.depth_mask=3; frame.engine_units_per_metre=2;
    frame.depth[0]=*metric; frame.depth[1]=*reversed;
    check(writer.Write(frame) && reader.Read(1,received) && received.structure_size==384 &&
        received.depth_mask==3 && close(received.depth[0].near_z,0.25F) &&
        close(received.depth[1].near_z,250) && received.engine_units_per_metre==2,
        "both independent metric mappings share the color/HUD/pose fence");
    for (unsigned change=0;change<12;++change) {
        auto bad=frame;
        switch(change) {
        case 0: bad.magic=0x3453544BU; break;
        case 1: bad.structure_size=336; break;
        case 2: bad.depth_mask=1; break;
        case 3: bad.depth_mask=0; break; // stale fields behind absence
        case 4: bad.depth[1].near_z=0; break;
        case 5: bad.depth[1].far_z=bad.depth[1].near_z; break;
        case 6: bad.depth[1].max_depth=bad.depth[1].min_depth; break;
        case 7: bad.depth[1].min_depth=std::numeric_limits<float>::quiet_NaN(); break;
        case 8: bad.engine_units_per_metre=0; break;
        case 9: bad.depth_reserved=1; break;
        case 10: bad.request.header.session_nonce.low^=1; break;
        case 11: bad.camera_frame_id=0; break;
        }
        check(!writer.Write(bad),"old version, partial/stale depth or foreign frame rejected before publication");
        check(reader.Read(1,received) && received.depth_mask==3 && received.camera_frame_id==99,
            "invalid write never replaces the preceding valid pair");
    }
    frame.depth_mask=0; frame.engine_units_per_metre=0; frame.depth[0]={}; frame.depth[1]={};
    check(writer.Write(frame) && reader.Read(1,received) && received.depth_mask==0 &&
        received.depth[0].near_z==0 && received.depth[1].far_z==0,
        "next color-only pair clears old metric mappings atomically");
    static_assert(ipc::StereoDepthOffset(1668)==3204);
    static_assert(ipc::StereoAtlasHeight(1668)==4872);
    frame.ready_value=4; frame.request.views[0].pose.position_x=0.5F;
    check(writer.Write(frame) && !reader.Read(1,received) && reader.Read(4,received) &&
        close(received.request.views[0].pose.position_x,0.5F),"ring reuse cannot return stale poses for a new image");
    std::cout<<"Native stereo tests: "<<failures<<" failures; metadata="<<sizeof(frame)<<" bytes\n";
    return failures ? 1:0;
}
