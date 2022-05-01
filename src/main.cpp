extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface * a_skse, SKSE::PluginInfo * a_info)
{
#ifndef NDEBUG
    auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
    auto path = logger::log_directory();
    if (!path) {
        return false;
    }

    *path /= "SinkOrSwim.log"sv;
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
    log->set_level(spdlog::level::trace);
#else
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::warn);
#endif

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

    logger::info("SinkOrSwim v1.0.0");

    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "SinkOrSwim";
    a_info->version = 1;

    if (a_skse->IsEditor()) {
        logger::critical("Loaded in editor, marking as incompatible"sv);
        return false;
    }

    const auto ver = a_skse->RuntimeVersion();
    if (ver < SKSE::RUNTIME_1_5_39) {
        logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
        return false;
    }

    return true;
}

static bool isHeavy = FALSE;

class Loki_SinkOrSwim
{
public:
    void* CodeAllocation(Xbyak::CodeGenerator& a_code, SKSE::Trampoline* t_ptr)
    {
        auto result = t_ptr->allocate(a_code.getSize());
        std::memcpy(result, a_code.getCode(), a_code.getSize());
        return result;

    }
    static void InstallSwimmingHook()
    {
        REL::Relocation<std::uintptr_t> target{ REL::ID(36357), 0x6ED };  // actor_update code insertion point
        REL::Relocation<std::uintptr_t> isSwimmingVariable{ REL::ID(241932) };
        Loki_SinkOrSwim L_SOS;

        struct Patch : Xbyak::CodeGenerator
        {
            Patch(std::uintptr_t a_variable, std::uintptr_t a_target)
            {
                Xbyak::Label _OurReturn;
                Xbyak::Label retLabel;
                Xbyak::Label isSwimmingAddr;

                setae(r13b);
                mov(rcx, (uintptr_t)&isHeavy);
                cmp(byte[rcx], 1);
                jne(retLabel);
                mov(r13b, 0);   // set kIsSwimming to 0 depending on if our own isHeavy is set to TRUE

                L(retLabel);
                mov(rcx, ptr[rip + isSwimmingAddr]);
                comiss(xmm6, ptr[rcx]);     // this is the games addr for checking if we are at the drowning level
                jmp(ptr[rip + _OurReturn]); // we do not touch the drowning flag or this address, but we overwrite
                                            // part of the code so we need to bring it into our injection
                L(isSwimmingAddr);          // all of this happens directly AFTER GetSubmergedLevel()
                dq(a_variable);

                L(_OurReturn);
                dq(a_target + 0xB);  // 0x6F8 - 0x6ED
            }
        };

        Patch patch(isSwimmingVariable.address(), target.address());
        patch.ready();

        auto& trampoline = SKSE::GetTrampoline();
        trampoline.write_branch<6>(target.address(), L_SOS.CodeAllocation(patch, &trampoline));
    }

    static void InstallWaterHook()
    {
        REL::Relocation<std::uintptr_t> ActorUpdate{ REL::ID(36357) };

        auto& trampoline = SKSE::GetTrampoline();
        _GetSubmergeLevel = trampoline.write_call<5>(ActorUpdate.address() + 0x6D3, GetSubmergeLevel);
    }

private:
    static float GetSubmergeLevel(RE::Actor* a_this, float a_zPos, RE::TESObjectCELL* a_cell)
    {
        float submergedLevel = _GetSubmergeLevel(a_this, a_zPos, a_cell);  // call to the OG

        static RE::SpellItem* WaterSlowdownSmol = NULL;
        static RE::SpellItem* WaterSlowdownBeeg = NULL;
        static RE::SpellItem* WaterSlowdownSwim = NULL;
        static RE::SpellItem* WaterSlowdownSink = NULL;
        static RE::TESDataHandler* dataHandle = NULL;

        if (!dataHandle) {  // we only need this to run once
            dataHandle = RE::TESDataHandler::GetSingleton();
            if (dataHandle) {
                WaterSlowdownSmol = dataHandle->LookupForm<RE::SpellItem>(0xD64, "SinkOrSwim.esp");
                WaterSlowdownBeeg = dataHandle->LookupForm<RE::SpellItem>(0xD65, "SinkOrSwim.esp");
                WaterSlowdownSwim = dataHandle->LookupForm<RE::SpellItem>(0xD67, "SinkOrSwim.esp");
                WaterSlowdownSink = dataHandle->LookupForm<RE::SpellItem>(0xD69, "SinkOrSwim.esp");
            }
        };

        auto activeEffects = a_this->GetActiveEffectList();
        isHeavy = FALSE;  // set to false on logic start
        if (submergedLevel >= 0.69) {
            a_this->RemoveSpell(WaterSlowdownBeeg);
            a_this->RemoveSpell(WaterSlowdownSmol);
            a_this->AddSpell(WaterSlowdownSwim);
            a_this->AddSpell(WaterSlowdownSink);

            if (activeEffects) {
                for (auto& ae : *activeEffects) {
                    if (ae && ae->spell == WaterSlowdownSink) {
                        if (ae->flags.none(RE::ActiveEffect::Flag::kInactive) && ae->flags.none(RE::ActiveEffect::Flag::kDispelled)) {
                            if (!a_this->GetCharController()) {
                                logger::info("Invalid CharController ptr, skipping gravity code.");
                            } else {
                                a_this->GetCharController()->gravity = 0.20f;    // set gravity so we "float" when submerged, dont let it reset
                                isHeavy = TRUE;
                                if (!a_this->pad11) {  // we only need this to run ONCE when meeting the condition
                                    const RE::hkVector4 hkv = { -1.00f, -1.00f, -1.00f, -1.00f };
                                    a_this->GetCharController()->SetLinearVelocityImpl(hkv);
                                    a_this->pad11 = TRUE;  // this is a (presumably) unused variable that i am putting to use
                                    goto JustFuckingLeave;
                                }
                            }
                        }
                    }
                }
            }
        } else if (submergedLevel >= 0.43) {  // everything below seems pretty self-explanatory so i dont feel the need to comment on it
            a_this->RemoveSpell(WaterSlowdownSmol);
            a_this->RemoveSpell(WaterSlowdownSwim);
            a_this->RemoveSpell(WaterSlowdownSink);
            a_this->AddSpell(WaterSlowdownBeeg);
            if (!a_this->GetCharController()) {
                logger::info("Invalid CharController ptr, skipping gravity code.");
            } else {
                a_this->GetCharController()->gravity = 1.00f;
            };
        } else if (submergedLevel >= 0.18) {
            a_this->RemoveSpell(WaterSlowdownBeeg);
            a_this->RemoveSpell(WaterSlowdownSwim);
            a_this->RemoveSpell(WaterSlowdownSink);
            a_this->AddSpell(WaterSlowdownSmol);
            if (!a_this->GetCharController()) {
                logger::info("Invalid CharController ptr, skipping gravity code.");
            } else {
                a_this->GetCharController()->gravity = 1.00f;
            };
        } else {
            a_this->RemoveSpell(WaterSlowdownSwim);
            a_this->RemoveSpell(WaterSlowdownBeeg);
            a_this->RemoveSpell(WaterSlowdownSmol);
            a_this->RemoveSpell(WaterSlowdownSink);
            if (!a_this->GetCharController()) {
                logger::info("Invalid CharController ptr, skipping gravity code.");
            } else {
                a_this->GetCharController()->gravity = 1.00f;
                a_this->pad11 = FALSE;  // set this to false when NOT meeting our condition
            };
        }
    JustFuckingLeave:
        return submergedLevel;
    };
    static inline REL::Relocation<decltype(GetSubmergeLevel)> _GetSubmergeLevel;
};


extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface * a_skse)
{
    logger::info("SinkOrSwim loaded");

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(128);

    Loki_SinkOrSwim::InstallWaterHook();
    Loki_SinkOrSwim::InstallSwimmingHook();

    return true;
}