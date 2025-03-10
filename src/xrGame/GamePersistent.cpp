#include "pch_script.h"
#include "GamePersistent.h"
#include "xrCore/FMesh.hpp"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/GameMtlLib.h"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/profiler.h"
#include "MainMenu.h"
#include "xrUICore/Cursor/UICursor.h"
#include "game_base_space.h"
#include "Level.h"
#include "ParticlesObject.h"
#include "game_base_space.h"
#include "stalker_animation_data_storage.h"
#include "stalker_velocity_holder.h"

#include "ActorEffector.h"
#include "Actor.h"
#include "Spectator.h"

#include "xrUICore/XML/UITextureMaster.h"

#include "xrEngine/xrSASH.h"
#include "ai_space.h"
#include "xrScriptEngine/script_engine.hpp"

#include "holder_custom.h"
#include "game_cl_base.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServerEntities/xrServer_Object_Base.h"
#include "ui/UIGameTutorial.h"
#include "xrEngine/GameFont.h"
#include "xrEngine/PerformanceAlert.hpp"
#include "xrEngine/xr_input.h"
#include "xrEngine/x_ray.h"
#include "ui/UILoadingScreen.h"
#include "AnselManager.h"
#include "xrCore/Threading/TaskManager.hpp"

#include "xrPhysics/IPHWorld.h"

#ifndef MASTER_GOLD
#include "CustomMonster.h"
#endif // MASTER_GOLD

#ifndef _EDITOR
#include "ai_debug.h"
#endif // _EDITOR

#include "xrEngine/xr_level_controller.h"

u32 UIStyleID = 0;
xr_vector<xr_token> UIStyleToken;

void FillUIStyleToken()
{
    UIStyleToken.emplace_back("ui_style_default", 0);

    string_path path;
    strconcat(sizeof(path), path, UI_PATH, DELIMITER "styles" DELIMITER);
    FS.update_path(path, _game_config_, path);
    auto styles = FS.file_list_open(path, FS_ListFolders | FS_RootOnly);
    if (styles != nullptr)
    {
        int i = 1; // It's 1, because 0 is default style
        for (const auto& style : *styles)
        {
            const auto pos = strchr(style, _DELIMITER);
            *pos = '\0'; // we don't need that backslash in the end
            UIStyleToken.emplace_back(xr_strdup(style), i++); // It's important to have postfix increment!
        }
        FS.file_list_close(styles);
    }

    UIStyleToken.emplace_back(nullptr, -1);
}

bool defaultUIStyle = true;
void SetupUIStyle()
{
    if (UIStyleID == 0)
    {
        if (!defaultUIStyle)
        {
            xr_free(UI_PATH);
            xr_free(UI_PATH_WITH_DELIMITER);
        }
        UI_PATH = UI_PATH_DEFAULT;
        UI_PATH_WITH_DELIMITER = UI_PATH_DEFAULT_WITH_DELIMITER;
        defaultUIStyle = true;
        return;
    }

    pcstr selectedStyle = nullptr;
    for (const auto& token : UIStyleToken)
        if (token.id == UIStyleID)
            selectedStyle = token.name;

    string_path selectedStylePath;
    strconcat(selectedStylePath, UI_PATH_DEFAULT, DELIMITER "styles" DELIMITER, selectedStyle);
    UI_PATH = xr_strdup(selectedStylePath);

    xr_strcat(selectedStylePath, DELIMITER);
    UI_PATH_WITH_DELIMITER = xr_strdup(selectedStylePath);

    defaultUIStyle = false;
}

void CleanupUIStyleToken()
{
    for (auto& token : UIStyleToken)
    {
        if (token.name && token.id != 0)
            xr_free(token.name);
    }
    UIStyleToken.clear();
    if (!defaultUIStyle)
    {
        xr_free(UI_PATH);
        xr_free(UI_PATH_WITH_DELIMITER);
    }
}

CGamePersistent::CGamePersistent(void)
{
    m_bPickableDOF = false;
    m_game_params.m_e_game_type = eGameIDNoGame;
    ambient_effect_next_time = 0;
    ambient_effect_stop_time = 0;
    ambient_particles = nullptr;

    ambient_effect_wind_start = 0.f;
    ambient_effect_wind_in_time = 0.f;
    ambient_effect_wind_end = 0.f;
    ambient_effect_wind_out_time = 0.f;
    ambient_effect_wind_on = false;

    ambient_sound_next_time.reserve(32);

    m_pMainMenu = nullptr;
    m_intro = nullptr;
    m_intro_event.bind(this, &CGamePersistent::start_logo_intro);
#ifdef DEBUG
    m_frame_counter = 0;
    m_last_stats_frame = u32(-2);
#endif

    const bool bDemoMode = (0 != strstr(Core.Params, "-demomode "));
    if (bDemoMode)
    {
        string256 fname;
        LPCSTR name = strstr(Core.Params, "-demomode ") + 10;
        sscanf(name, "%s", fname);
        R_ASSERT2(fname[0], "Missing filename for 'demomode'");
        Msg("- playing in demo mode '%s'", fname);
        pDemoFile = FS.r_open(fname);
        Device.seqFrame.Add(this);
        eDemoStart = Engine.Event.Handler_Attach("GAME:demo", this);
        uTime2Change = 0;
    }
    else
    {
        pDemoFile = NULL;
        eDemoStart = NULL;
    }

    eQuickLoad = Engine.Event.Handler_Attach("Game:QuickLoad", this);
    Fvector3* DofValue = Console->GetFVectorPtr("r2_dof");
    SetBaseDof(*DofValue);
}

CGamePersistent::~CGamePersistent(void)
{
    FS.r_close(pDemoFile);
    Device.seqFrame.Remove(this);
    Engine.Event.Handler_Detach(eDemoStart, this);
    Engine.Event.Handler_Detach(eQuickLoad, this);
}

void CGamePersistent::PreStart(LPCSTR op)
{
    inherited::PreStart(op);
}

void CGamePersistent::RegisterModel(IRenderVisual* V)
{
    // Check types
    switch (V->getType())
    {
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        u16 def_idx = GMLib.GetMaterialIdx("default_object");
        R_ASSERT2(GMLib.GetMaterialByIdx(def_idx)->Flags.is(SGameMtl::flDynamic), "'default_object' - must be dynamic");
        IKinematics* K = smart_cast<IKinematics*>(V);
        VERIFY(K);
        const u16 cnt = K->LL_BoneCount();
        for (u16 k = 0; k < cnt; k++)
        {
            CBoneData& bd = K->LL_GetData(k);
            if (*(bd.game_mtl_name))
            {
                bd.game_mtl_idx = GMLib.GetMaterialIdx(*bd.game_mtl_name);
                R_ASSERT2(GMLib.GetMaterialByIdx(bd.game_mtl_idx)->Flags.is(SGameMtl::flDynamic),
                    "Required dynamic game material");
            }
            else
            {
                bd.game_mtl_idx = def_idx;
            }
        }
    }
    break;
    }
}

extern void clean_game_globals();
extern void init_game_globals();

void CGamePersistent::create_main_menu(Task&, void*)
{
    m_pMainMenu = xr_new<CMainMenu>();
}

void CGamePersistent::OnAppStart()
{
    // init game globals
#ifndef XR_PLATFORM_WINDOWS
    init_game_globals();
#else
    const auto& initializeGlobals = TaskScheduler->AddTask("init_game_globals()", [](Task&, void*)
    {
        init_game_globals();
    });
#endif
    // load game materials
    const auto& loadMaterials = TaskScheduler->AddTask("GMLib.Load()", [](Task&, void*)
    {
        IRender::ScopedContext context(IRender::HelperContext);
        GMLib.Load();
    });

    SetupUIStyle();
    GEnv.UI = xr_new<UICore>();

    const auto& menuCreated = TaskScheduler->AddTask("CMainMenu::CMainMenu()", { this, &CGamePersistent::create_main_menu });

    inherited::OnAppStart();

    if (!GEnv.isDedicatedServer)
        pApp->SetLoadingScreen(xr_new<UILoadingScreen>());

#ifdef XR_PLATFORM_WINDOWS
    ansel = xr_new<AnselManager>();
    ansel->Load();
    ansel->Init();
#endif

#ifdef XR_PLATFORM_WINDOWS
    TaskScheduler->Wait(initializeGlobals);
#endif
    TaskScheduler->Wait(loadMaterials);
    TaskScheduler->Wait(menuCreated);
}

void CGamePersistent::OnAppEnd()
{
    if (m_pMainMenu->IsActive())
        m_pMainMenu->Activate(false);

    pApp->DestroyLoadingScreen();
    xr_delete(m_pMainMenu);
    xr_delete(GEnv.UI);

    inherited::OnAppEnd();

    clean_game_globals();

    GMLib.Unload();

#ifdef XR_PLATFORM_WINDOWS
    xr_delete(ansel);
#endif
}

void CGamePersistent::Start(LPCSTR op) { inherited::Start(op); }
void CGamePersistent::Disconnect()
{
    // destroy ambient particles
    CParticlesObject::Destroy(ambient_particles);

    inherited::Disconnect();
    // stop all played emitters
    GEnv.Sound->stop_emitters();
    m_game_params.m_e_game_type = eGameIDNoGame;
}

void CGamePersistent::OnGameStart()
{
    inherited::OnGameStart();
    UpdateGameType();
}

LPCSTR GameTypeToString(EGameIDs gt, bool bShort)
{
    switch (gt)
    {
    case eGameIDSingle:             return "single";
    case eGameIDDeathmatch:         return (bShort) ? "dm"  : "deathmatch";
    case eGameIDTeamDeathmatch:     return (bShort) ? "tdm" : "teamdeathmatch";
    case eGameIDArtefactHunt:       return (bShort) ? "ah"  : "artefacthunt";
    case eGameIDCaptureTheArtefact: return (bShort) ? "cta" : "capturetheartefact";
    case eGameIDDominationZone:     return (bShort) ? "dz"  : "dominationzone";
    case eGameIDTeamDominationZone: return (bShort) ? "tdz" : "teamdominationzone";
    default: return "---";
    }
}

LPCSTR GameTypeToStringEx(u32 gt, bool bShort)
{
    switch (gt)
    {
    case eGameIDTeamDeathmatch_SoC: gt = eGameIDTeamDeathmatch; break;
    case eGameIDArtefactHunt_SoC: gt = eGameIDArtefactHunt; break;
    }
    return GameTypeToString(static_cast<EGameIDs>(gt), bShort);
}

void CGamePersistent::UpdateGameType()
{
    inherited::UpdateGameType();

    m_game_params.m_e_game_type = ParseStringToGameType(m_game_params.m_game_type);

    if (m_game_params.m_e_game_type == eGameIDSingle)
        g_current_keygroup = _sp;
    else
        g_current_keygroup = _mp;
}

void CGamePersistent::OnGameEnd()
{
    inherited::OnGameEnd();

    xr_delete(g_stalker_animation_data_storage);
    xr_delete(g_stalker_velocity_holder);
}

void CGamePersistent::WeathersUpdate()
{
    if (g_pGameLevel && !GEnv.isDedicatedServer)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
        BOOL bIndoor = TRUE;
        if (actor)
            bIndoor = actor->renderable_ROS()->get_luminocity_hemi() < 0.05f;

        const size_t data_set = (Random.randF() < (1.f - Environment().CurrentEnv->weight)) ? 0 : 1;

        CEnvDescriptor* const _env = Environment().Current[data_set];
        VERIFY(_env);

        if (CEnvAmbient* env_amb = _env->env_ambient; env_amb)
        {
            CEnvAmbient::SSndChannelVec& vec = env_amb->get_snd_channels();

            auto I = vec.cbegin();
            const auto E = vec.cend();

            for (size_t idx = 0; I != E; ++I, ++idx)
            {
                CEnvAmbient::SSndChannel& ch = **I;

                if (ambient_sound_next_time[idx] == 0) // first
                {
                    ambient_sound_next_time[idx] = Device.dwTimeGlobal + ch.get_rnd_sound_first_time();
                }
                else if (Device.dwTimeGlobal > ambient_sound_next_time[idx])
                {
                    ref_sound& snd = ch.get_rnd_sound();

                    Fvector pos;
                    const float angle = ::Random.randF(PI_MUL_2);
                    pos.x = _cos(angle);
                    pos.y = 0;
                    pos.z = _sin(angle);
                    pos.normalize().mul(ch.get_rnd_sound_dist()).add(Device.vCameraPosition);
                    pos.y += 10.f;
                    snd.play_at_pos(nullptr, pos);

#ifdef DEBUG
                    if (!snd._handle() && strstr(Core.Params, "-nosound"))
                        continue;
#endif // DEBUG

                    VERIFY(snd._handle());
                    const u32 _length_ms = iFloor(snd.get_length_sec() * 1000.0f);
                    ambient_sound_next_time[idx] = Device.dwTimeGlobal + _length_ms + ch.get_rnd_sound_time();
                    //Msg("- Playing ambient sound channel [%s] file[%s]", ch.m_load_section.c_str(), snd._handle()->file_name());
                }
            }

            // start effect
            if ((FALSE == bIndoor) && (0 == ambient_particles) && Device.dwTimeGlobal > ambient_effect_next_time)
            {
                if (CEnvAmbient::SEffect* eff = env_amb->get_rnd_effect(); eff)
                {
                    Environment().wind_gust_factor = eff->wind_gust_factor;
                    ambient_effect_next_time = Device.dwTimeGlobal + env_amb->get_rnd_effect_time();
                    ambient_effect_stop_time = Device.dwTimeGlobal + eff->life_time;
                    ambient_effect_wind_start = Device.fTimeGlobal;
                    ambient_effect_wind_in_time = Device.fTimeGlobal + eff->wind_blast_in_time;
                    ambient_effect_wind_end = Device.fTimeGlobal + eff->life_time / 1000.f;
                    ambient_effect_wind_out_time =
                        Device.fTimeGlobal + eff->life_time / 1000.f + eff->wind_blast_out_time;
                    ambient_effect_wind_on = true;

                    ambient_particles = CParticlesObject::Create(eff->particles.c_str(), FALSE, false);
                    Fvector pos;
                    pos.add(Device.vCameraPosition, eff->offset);
                    ambient_particles->play_at_pos(pos);
                    if (eff->sound._handle())
                        eff->sound.play_at_pos(0, pos);

                    Environment().wind_blast_strength_start_value = Environment().wind_strength_factor;
                    Environment().wind_blast_strength_stop_value = eff->wind_blast_strength;

                    if (Environment().wind_blast_strength_start_value == 0.f)
                    {
                        Environment().wind_blast_start_time.set(
                            0.f, eff->wind_blast_direction.x, eff->wind_blast_direction.y, eff->wind_blast_direction.z);
                    }
                    else
                    {
                        Environment().wind_blast_start_time.set(0.f, Environment().wind_blast_direction.x,
                            Environment().wind_blast_direction.y, Environment().wind_blast_direction.z);
                    }
                    Environment().wind_blast_stop_time.set(
                        0.f, eff->wind_blast_direction.x, eff->wind_blast_direction.y, eff->wind_blast_direction.z);
                }
            }
        }
        if (Device.fTimeGlobal >= ambient_effect_wind_start && Device.fTimeGlobal <= ambient_effect_wind_in_time &&
            ambient_effect_wind_on)
        {
            const float delta = ambient_effect_wind_in_time - ambient_effect_wind_start;
            float t;
            if (delta != 0.f)
            {
                const float cur_in = Device.fTimeGlobal - ambient_effect_wind_start;
                t = cur_in / delta;
            }
            else
            {
                t = 0.f;
            }
            Environment().wind_blast_current.slerp(
                Environment().wind_blast_start_time, Environment().wind_blast_stop_time, t);

            Environment().wind_blast_direction.set(Environment().wind_blast_current.x,
                Environment().wind_blast_current.y, Environment().wind_blast_current.z);
            Environment().wind_strength_factor = Environment().wind_blast_strength_start_value +
                t * (Environment().wind_blast_strength_stop_value - Environment().wind_blast_strength_start_value);
        }

        // stop if time exceed or indoor
        if (bIndoor || Device.dwTimeGlobal >= ambient_effect_stop_time)
        {
            if (ambient_particles)
                ambient_particles->Stop();

            Environment().wind_gust_factor = 0.f;
        }

        if (Device.fTimeGlobal >= ambient_effect_wind_end && ambient_effect_wind_on)
        {
            Environment().wind_blast_strength_start_value = Environment().wind_strength_factor;
            Environment().wind_blast_strength_stop_value = 0.f;

            ambient_effect_wind_on = false;
        }

        if (Device.fTimeGlobal >= ambient_effect_wind_end && Device.fTimeGlobal <= ambient_effect_wind_out_time)
        {
            const float delta = ambient_effect_wind_out_time - ambient_effect_wind_end;
            float t;
            if (delta != 0.f)
            {
                const float cur_in = Device.fTimeGlobal - ambient_effect_wind_end;
                t = cur_in / delta;
            }
            else
            {
                t = 0.f;
            }
            Environment().wind_strength_factor = Environment().wind_blast_strength_start_value +
                t * (Environment().wind_blast_strength_stop_value - Environment().wind_blast_strength_start_value);
        }
        if (Device.fTimeGlobal > ambient_effect_wind_out_time && ambient_effect_wind_out_time != 0.f)
        {
            Environment().wind_strength_factor = 0.0;
        }

        // if particles not playing - destroy
        if (ambient_particles && !ambient_particles->IsPlaying())
            CParticlesObject::Destroy(ambient_particles);
    }
}

bool allow_intro()
{
#if defined(XR_PLATFORM_WINDOWS)
    if ((0 != strstr(Core.Params, "-nointro")) || g_SASH.IsRunning())
#else
    if (0 != strstr(Core.Params, "-nointro"))
#endif
    {
        return false;
    }
    else
        return true;
}

bool allow_game_intro()
{
    return !strstr(Core.Params, "-nogameintro");
}

void CGamePersistent::start_logo_intro()
{
    if (!allow_intro())
    {
        m_intro_event = nullptr;
        Console->Show();
        Console->Execute("main_menu on");
        return;
    }

    if (Device.dwPrecacheFrame == 0)
    {
        m_intro_event = nullptr;
        if (!GEnv.isDedicatedServer && 0 == xr_strlen(m_game_params.m_game_or_spawn) && NULL == g_pGameLevel)
        {
            VERIFY(NULL == m_intro);
            m_intro = xr_new<CUISequencer>();
            m_intro->m_on_destroy_event.bind(this, &CGamePersistent::update_logo_intro);
            m_intro->Start("intro_logo");
            Console->Hide();
        }
    }
}

void CGamePersistent::update_logo_intro()
{
    xr_delete(m_intro);
    Console->Execute("main_menu on");
}

extern int g_keypress_on_start;
void CGamePersistent::game_loaded()
{
    if (Device.dwPrecacheFrame <= 2)
    {
        m_intro_event = nullptr;
        if (g_pGameLevel && g_pGameLevel->bReady && (allow_game_intro() && g_keypress_on_start) &&
            load_screen_renderer.NeedsUserInput() && m_game_params.m_e_game_type == eGameIDSingle)
        {
            VERIFY(NULL == m_intro);
            m_intro = xr_new<CUISequencer>();
            m_intro->m_on_destroy_event.bind(this, &CGamePersistent::update_game_loaded);
            if (!m_intro->Start("game_loaded"))
                m_intro->Destroy();
        }
    }
}

void CGamePersistent::update_game_loaded()
{
    xr_delete(m_intro);
    load_screen_renderer.Stop();
    start_game_intro();
}

void CGamePersistent::start_game_intro()
{
    if (!allow_intro())
    {
        return;
    }

    if (g_pGameLevel && g_pGameLevel->bReady && Device.dwPrecacheFrame <= 2)
    {
        if (0 == xr_stricmp(m_game_params.m_new_or_load, "new"))
        {
            VERIFY(NULL == m_intro);
            Log("intro_start intro_game");
            m_intro = xr_new<CUISequencer>();
            m_intro->m_on_destroy_event.bind(this, &CGamePersistent::update_game_intro);
            m_intro->Start("intro_game");
        }
    }
}

void CGamePersistent::update_game_intro()
{
    xr_delete(m_intro);
}

extern CUISequencer* g_tutorial;
extern CUISequencer* g_tutorial2;

void CGamePersistent::OnFrame()
{
    if (Device.dwPrecacheFrame == 5 && m_intro_event.empty())
    {
        SetLoadStageTitle();
        m_intro_event.bind(this, &CGamePersistent::game_loaded);
    }

    if (g_tutorial2)
    {
        g_tutorial2->Destroy();
        xr_delete(g_tutorial2);
    }

    if (g_tutorial && !g_tutorial->IsActive())
    {
        xr_delete(g_tutorial);
    }
    if (0 == Device.dwFrame % 200)
        CUITextureMaster::FreeCachedShaders();

#ifdef DEBUG
    ++m_frame_counter;
#endif
    if (!GEnv.isDedicatedServer)
    {
        if (!m_intro_event.empty())
            m_intro_event();
        else if (!m_intro)
        {
            if (Device.dwPrecacheFrame == 0)
                load_screen_renderer.Stop();
        }
    }
    if (!m_pMainMenu->IsActive())
        m_pMainMenu->DestroyInternal(false);

    if (!g_pGameLevel)
        return;
    if (!g_pGameLevel->bReady)
        return;

    g_pGameLevel->WorldRendered(false);

    if (Device.Paused())
    {
        if (Level().IsDemoPlay())
        {
            CSpectator* tmp_spectr = smart_cast<CSpectator*>(Level().CurrentControlEntity());
            if (tmp_spectr)
            {
                tmp_spectr->UpdateCL(); // updating spectator in pause (pause ability of demo play)
            }
        }
#ifndef MASTER_GOLD
        if (Level().CurrentViewEntity() && IsGameTypeSingle())
        {
            if (!g_actor || (g_actor->ID() != Level().CurrentViewEntity()->ID()))
            {
                CCustomMonster* custom_monster = smart_cast<CCustomMonster*>(Level().CurrentViewEntity());
                if (custom_monster) // can be spectator in multiplayer
                    custom_monster->UpdateCamera();
            }
            else
            {
                CCameraBase* C = NULL;
                if (g_actor)
                {
                    if (!Actor()->Holder())
                        C = Actor()->cam_Active();
                    else
                        C = Actor()->Holder()->Camera();

                    Actor()->Cameras().UpdateFromCamera(C);
                    Actor()->Cameras().ApplyDevice();

                    if (psActorFlags.test(AF_NO_CLIP))
                    {
#ifdef DEBUG
                        Actor()->SetDbgUpdateFrame(0);
                        Actor()->GetSchedulerData().dbg_update_shedule = 0;
#endif
                        Device.dwTimeDelta = 0;
                        Device.fTimeDelta = 0.01f;
                        Actor()->UpdateCL();
                        Actor()->shedule_Update(0);
#ifdef DEBUG
                        Actor()->SetDbgUpdateFrame(0);
                        Actor()->GetSchedulerData().dbg_update_shedule = 0;
#endif

                        CSE_Abstract* e = Level().Server->ID_to_entity(Actor()->ID());
                        VERIFY(e);
                        CSE_ALifeCreatureActor* s_actor = smart_cast<CSE_ALifeCreatureActor*>(e);
                        VERIFY(s_actor);
                        xr_vector<u16>::iterator it = s_actor->children.begin();
                        for (; it != s_actor->children.end(); ++it)
                        {
                            IGameObject* obj = Level().Objects.net_Find(*it);
                            if (obj && Engine.Sheduler.Registered(obj))
                            {
#ifdef DEBUG
                                obj->GetSchedulerData().dbg_update_shedule = 0;
                                obj->SetDbgUpdateFrame(0);
#endif
                                obj->shedule_Update(0);
                                obj->UpdateCL();
#ifdef DEBUG
                                obj->GetSchedulerData().dbg_update_shedule = 0;
                                obj->SetDbgUpdateFrame(0);
#endif
                            }
                        }
                    }
                }
            }
        }
#else // MASTER_GOLD
        if (g_actor && IsGameTypeSingle())
        {
            CCameraBase* C = NULL;
            if (!Actor()->Holder())
                C = Actor()->cam_Active();
            else
                C = Actor()->Holder()->Camera();

            Actor()->Cameras().UpdateFromCamera(C);
            Actor()->Cameras().ApplyDevice();
        }
#endif // MASTER_GOLD
    }
    inherited::OnFrame();

    if (!Device.Paused())
    {
        Engine.Sheduler.Update();
    }

    // update weathers ambient
    if (!Device.Paused())
        WeathersUpdate();

    if (0 != pDemoFile)
    {
        if (Device.dwTimeGlobal > uTime2Change)
        {
            // Change level + play demo
            if (pDemoFile->elapsed() < 3)
                pDemoFile->seek(0); // cycle

            // Read params
            string512 params;
            pDemoFile->r_string(params, sizeof(params));
            string256 o_server, o_client, o_demo;
            u32 o_time;
            sscanf(params, "%[^,],%[^,],%[^,],%d", o_server, o_client, o_demo, &o_time);

            // Start _new level + demo
            Engine.Event.Defer("KERNEL:disconnect");
            Engine.Event.Defer("KERNEL:start", size_t(xr_strdup(_Trim(o_server))), size_t(xr_strdup(_Trim(o_client))));
            Engine.Event.Defer("GAME:demo", size_t(xr_strdup(_Trim(o_demo))), u64(o_time));
            uTime2Change = 0xffffffff; // Block changer until Event received
        }
    }

#ifdef DEBUG
    if ((m_last_stats_frame + 1) < m_frame_counter)
        profiler().clear();
#endif
    UpdateDof();
}

#include "game_sv_single.h"
#include "xrServer.h"
#include "UIGameCustom.h"
#include "ui/UIMainIngameWnd.h"
#include "ui/UIPdaWnd.h"

void CGamePersistent::OnEvent(EVENT E, u64 P1, u64 P2)
{
    if (E == eQuickLoad)
    {
        if (Device.Paused())
            Device.Pause(FALSE, TRUE, TRUE, "eQuickLoad");

        if (CurrentGameUI())
        {
            CurrentGameUI()->HideShownDialogs();
            CurrentGameUI()->UIMainIngameWnd->reset_ui();
            CurrentGameUI()->GetPdaMenu().Reset();
        }

        if (g_tutorial)
            g_tutorial->Stop();

        if (g_tutorial2)
            g_tutorial2->Stop();

        pstr saved_name = (pstr)(P1);

        Level().remove_objects();
        game_sv_Single* game = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
        R_ASSERT(game);
        game->restart_simulator(saved_name);
        xr_free(saved_name);
        return;
    }
    else if (E == eDemoStart)
    {
        string256 cmd;
        LPCSTR demo = LPCSTR(P1);
        xr_sprintf(cmd, "demo_play %s", demo);
        Console->Execute(cmd);
        xr_free(demo);
        uTime2Change = Device.TimerAsync() + u32(P2) * 1000;
    }
}

void CGamePersistent::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    IGame_Persistent::DumpStatistics(font, alert);
    if (physics_world())
        physics_world()->DumpStatistics(font, alert);

#ifdef DEBUG
#ifndef _EDITOR
    const Fvector2 prev = font.GetPosition();
    font.OutSet(400, 120);
    m_last_stats_frame = m_frame_counter;
    profiler().show_stats(font, psAI_Flags.test(aiStats));
    font.OutSet(prev.x, prev.y);
#endif
#endif
}

float CGamePersistent::MtlTransparent(u32 mtl_idx)
{
    return GMLib.GetMaterialByIdx((u16)mtl_idx)->fVisTransparencyFactor;
}
static BOOL bRestorePause = FALSE;
static BOOL bEntryFlag = TRUE;

void CGamePersistent::OnAppActivate()
{
    if (psDeviceFlags.test(rsAlwaysActive))
        return;

    bool bIsMP = (g_pGameLevel && Level().game && GameID() != eGameIDSingle);
    bIsMP &= !Device.Paused();

    if (!bIsMP)
        Device.Pause(FALSE, !bRestorePause, TRUE, "CGP::OnAppActivate");
    else
        Device.Pause(FALSE, TRUE, TRUE, "CGP::OnAppActivate MP");

    bEntryFlag = TRUE;
}

void CGamePersistent::OnAppDeactivate()
{
    if (!bEntryFlag || psDeviceFlags.test(rsAlwaysActive))
        return;

    bool bIsMP = (g_pGameLevel && Level().game && GameID() != eGameIDSingle);

    bRestorePause = FALSE;

    if (!bIsMP)
    {
        bRestorePause = Device.Paused();
        Device.Pause(TRUE, TRUE, TRUE, "CGP::OnAppDeactivate");
    }
    else
    {
        Device.Pause(TRUE, FALSE, TRUE, "CGP::OnAppDeactivate MP");
    }
    bEntryFlag = FALSE;
}

bool CGamePersistent::OnRenderPPUI_query()
{
    return MainMenu()->OnRenderPPUI_query();
    // enable PP or not
}

extern void draw_wnds_rects();
void CGamePersistent::OnRenderPPUI_main()
{
    // always
    MainMenu()->OnRenderPPUI_main();
    draw_wnds_rects();
}

void CGamePersistent::OnRenderPPUI_PP() { MainMenu()->OnRenderPPUI_PP(); }

#include "xrEngine/x_ray.h"
void CGamePersistent::LoadTitle(bool change_tip, shared_str map_name)
{
    pApp->LoadStage();
    if (change_tip)
    {
        bool noTips = false;
        string512 buff;
        u8 tip_num;
        luabind::functor<u8> m_functor;
        const bool is_single = !xr_strcmp(m_game_params.m_game_type, "single");
        if (is_single)
        {
            if (GEnv.ScriptEngine->functor("loadscreen.get_tip_number", m_functor))
                tip_num = m_functor(map_name.c_str());
            else
                noTips = true;
        }
        else
        {
            if (GEnv.ScriptEngine->functor("loadscreen.get_mp_tip_number", m_functor))
                tip_num = m_functor(map_name.c_str());
            else
                noTips = true;
        }
        if (noTips)
            return;

        xr_sprintf(buff, "%s%d:", StringTable().translate("ls_tip_number").c_str(), tip_num);
        shared_str tmp = buff;

        if (is_single)
            xr_sprintf(buff, "ls_tip_%d", tip_num);
        else
            xr_sprintf(buff, "ls_mp_tip_%d", tip_num);

        pApp->LoadTitleInt(
            StringTable().translate("ls_header").c_str(), tmp.c_str(), StringTable().translate(buff).c_str());
    }
}

void CGamePersistent::SetLoadStageTitle(pcstr ls_title)
{
    string256 buff;
    if (ls_title)
    {
        xr_sprintf(buff, "%s%s", StringTable().translate(ls_title).c_str(), "...");
        pApp->SetLoadStageTitle(buff);
    }
    else
        pApp->SetLoadStageTitle("");
}

bool CGamePersistent::CanBePaused() { return IsGameTypeSingle() || (g_pGameLevel && Level().IsDemoPlay()); }
void CGamePersistent::SetPickableEffectorDOF(bool bSet)
{
    m_bPickableDOF = bSet;
    if (!bSet)
        RestoreEffectorDOF();
}

void CGamePersistent::GetCurrentDof(Fvector3& dof) { dof = m_dof[1]; }
void CGamePersistent::SetBaseDof(const Fvector3& dof) { m_dof[0] = m_dof[1] = m_dof[2] = m_dof[3] = dof; }
void CGamePersistent::SetEffectorDOF(const Fvector& needed_dof)
{
    if (m_bPickableDOF)
        return;
    m_dof[0] = needed_dof;
    m_dof[2] = m_dof[1]; // current
}

void CGamePersistent::RestoreEffectorDOF() { SetEffectorDOF(m_dof[3]); }
#include "HUDManager.h"

//	m_dof		[4];	// 0-dest 1-current 2-from 3-original
void CGamePersistent::UpdateDof()
{
    if (m_bPickableDOF)
    {
        static float diff_far = pSettings->read_if_exists<float>("zone_pick_dof", "far", 70.0f);
        static float diff_near = pSettings->read_if_exists<float>("zone_pick_dof", "near", -70.0f);

        Fvector pick_dof;
        pick_dof.y = HUD().GetCurrentRayQuery().range;
        pick_dof.x = pick_dof.y + diff_near;
        pick_dof.z = pick_dof.y + diff_far;
        m_dof[0] = pick_dof;
        m_dof[2] = m_dof[1]; // current
    }
    if (m_dof[1].similar(m_dof[0]))
        return;

    float td = Device.fTimeDelta;
    Fvector diff;
    diff.sub(m_dof[0], m_dof[2]);
    diff.mul(td / 0.2f); // 0.2 sec
    m_dof[1].add(diff);
    (m_dof[0].x < m_dof[2].x) ? clamp(m_dof[1].x, m_dof[0].x, m_dof[2].x) : clamp(m_dof[1].x, m_dof[2].x, m_dof[0].x);
    (m_dof[0].y < m_dof[2].y) ? clamp(m_dof[1].y, m_dof[0].y, m_dof[2].y) : clamp(m_dof[1].y, m_dof[2].y, m_dof[0].y);
    (m_dof[0].z < m_dof[2].z) ? clamp(m_dof[1].z, m_dof[0].z, m_dof[2].z) : clamp(m_dof[1].z, m_dof[2].z, m_dof[0].z);
}

void CGamePersistent::OnSectorChanged(int sector)
{
    if (CurrentGameUI())
        CurrentGameUI()->UIMainIngameWnd->OnSectorChanged(sector);
}

void CGamePersistent::OnAssetsChanged()
{
    IGame_Persistent::OnAssetsChanged();
    StringTable().rescan();
}
