
#pragma once

#include <JNIHelper.h>
#include <base/object.h>
#include <art/runtime/mirror/class.h>
#include <android-base/strings.h>
#include "runtime.h"
#include "config.h"
#include "jni_env_ext.h"
#include "edxp_context.h"
#include "jni/edxp_pending_hooks.h"
#include "utils.h"
#include "HookMain.h"

namespace art {

    class ClassLinker : public edxp::HookedObject {

    private:
        inline static ClassLinker *instance_;

        CREATE_FUNC_SYMBOL_ENTRY(void, SetEntryPointsToInterpreter, void *thiz, void *art_method) {
            if (LIKELY(SetEntryPointsToInterpreterSym))
                SetEntryPointsToInterpreterSym(thiz, art_method);
        }

        CREATE_HOOK_STUB_ENTRIES(void *, Constructor, void *thiz, void *intern_table) {
            LOGI("ConstructorReplace called");
            if (LIKELY(instance_))
                instance_->Reset(thiz);
            else
                instance_ = new ClassLinker(thiz);
            return ConstructorBackup(thiz, intern_table);
        }

        CREATE_HOOK_STUB_ENTRIES(void, FixupStaticTrampolines, void *thiz, void *clazz_ptr) {
            art::mirror::Class clazz(clazz_ptr);
            std::string storage;
            const char *desc = clazz.GetDescriptor(&storage);
            bool should_intercept =
                    edxp::IsClassPending(desc) || std::string(desc).rfind("LEdHooker_") == 0;
            if (UNLIKELY(should_intercept)) {
                edxp::Context::GetInstance()->CallOnPreFixupStaticTrampolines(clazz_ptr);
            }
            FixupStaticTrampolinesBackup(thiz, clazz_ptr);
            if (UNLIKELY(should_intercept)) {
                edxp::Context::GetInstance()->CallOnPostFixupStaticTrampolines(clazz_ptr);
            }
        }

        CREATE_FUNC_SYMBOL_ENTRY(void, MakeInitializedClassesVisiblyInitialized, void *thiz,
                                 void *self, bool wait) {
            if (LIKELY(MakeInitializedClassesVisiblyInitializedSym))
                MakeInitializedClassesVisiblyInitializedSym(thiz, self, wait);
        }


        CREATE_HOOK_STUB_ENTRIES(bool, ShouldUseInterpreterEntrypoint, void *art_method,
                                 const void *quick_code) {
            if (UNLIKELY(quick_code != nullptr && edxp::isEntryHooked(quick_code))) {
                return false;
            }
            return ShouldUseInterpreterEntrypointBackup(art_method, quick_code);
        }

        CREATE_HOOK_STUB_ENTRIES(bool, IsQuickToInterpreterBridge, void *thiz,
                                 const void *quick_code) {
            if (quick_code != nullptr && UNLIKELY(edxp::isEntryHooked(quick_code))) {
                LOGD("Pretend to be quick to interpreter bridge %p", quick_code);
                return true;
            }
            return IsQuickToInterpreterBridgeBackup(thiz, quick_code);
        }

    public:
        ClassLinker(void *thiz) : HookedObject(thiz) {}

        static ClassLinker *Current() {
            return instance_;
        }

        // @ApiSensitive(Level.MIDDLE)
        static void Setup(void *handle, HookFunType hook_func) {
            LOGD("Classlinker hook setup, handle=%p", handle);
            // TODO: Maybe not compatible with Android 10-
            int api_level = edxp::GetAndroidApiLevel();
            size_t OFFSET_classlinker;  // Get offset from art::Runtime::RunRootClinits() call in IDA
            switch (api_level) {
                case __ANDROID_API_O__:
                    [[fallthrough]];
                case __ANDROID_API_O_MR1__:
                    if constexpr(edxp::is64) {
                        OFFSET_classlinker = 464;
                    } else {
                        OFFSET_classlinker = 284;
                    }
                    break;
                case __ANDROID_API_P__:
                    if constexpr(edxp::is64) {
                        OFFSET_classlinker = 528;
                    } else {
                        OFFSET_classlinker = 336;
                    }
                    break;
                case __ANDROID_API_Q__:
                    if constexpr(edxp::is64) {
                        OFFSET_classlinker = 480;
                    } else {
                        OFFSET_classlinker = 280;
                    }
                    break;
                default:
                    LOGE("No valid offset for art::Runtime::class_linker_ found. Using Android R.");
                    [[fallthrough]];
                case __ANDROID_API_R__:
                    if constexpr(edxp::is64) {
                        OFFSET_classlinker = 472;
                    } else {
                        OFFSET_classlinker = 276;
                    }
                    break;
            }

            void *thiz = *reinterpret_cast<void **>(
                    reinterpret_cast<size_t>(Runtime::Current()->Get()) + OFFSET_classlinker);
            // ClassLinker* GetClassLinker() but inlined
            LOGD("Classlinker object: %p", thiz);
            instance_ = new ClassLinker(thiz);

            HOOK_FUNC(Constructor, "_ZN3art11ClassLinkerC2EPNS_11InternTableE",
                      "_ZN3art11ClassLinkerC2EPNS_11InternTableEb"); // 10.0
            RETRIEVE_FUNC_SYMBOL(SetEntryPointsToInterpreter,
                                 "_ZNK3art11ClassLinker27SetEntryPointsToInterpreterEPNS_9ArtMethodE");

            HOOK_FUNC(FixupStaticTrampolines,
                      "_ZN3art11ClassLinker22FixupStaticTrampolinesENS_6ObjPtrINS_6mirror5ClassEEE");

            // Sandhook will hook ShouldUseInterpreterEntrypoint, so we just skip
            // edxp::Context::GetInstance()->GetVariant() will not work here, so we use smh dirty hack
            if (api_level >= __ANDROID_API_Q__ &&
                edxp::path_exists(edxp::kLibSandHookNativePath)) {
                LOGD("Not sandhook, installing _ZN3art11ClassLinker30ShouldUseInterpreterEntrypointEPNS_9ArtMethodEPKv");
                HOOK_FUNC(ShouldUseInterpreterEntrypoint,
                          "_ZN3art11ClassLinker30ShouldUseInterpreterEntrypointEPNS_9ArtMethodEPKv");
            }

            // MakeInitializedClassesVisiblyInitialized will cause deadlock
            // IsQuickToInterpreterBridge cannot be hooked by Dobby yet
            // So we use GetSavedEntryPointOfPreCompiledMethod instead
//            if (api_level >= __ANDROID_API_R__) {
//                RETRIEVE_FUNC_SYMBOL(MakeInitializedClassesVisiblyInitialized,
//                                     "_ZN3art11ClassLinker40MakeInitializedClassesVisiblyInitializedEPNS_6ThreadEb");
//                HOOK_FUNC(IsQuickToInterpreterBridge,
//                          "_ZNK3art11ClassLinker26IsQuickToInterpreterBridgeEPKv");
//            }

        }

        ALWAYS_INLINE void MakeInitializedClassesVisiblyInitialized(void *self, bool wait) const {
            LOGD("MakeInitializedClassesVisiblyInitialized start, thiz=%p, self=%p", thiz_, self);
            if (LIKELY(thiz_))
                MakeInitializedClassesVisiblyInitialized(thiz_, self, wait);
        }

        ALWAYS_INLINE void SetEntryPointsToInterpreter(void *art_method) const {
            LOGD("SetEntryPointsToInterpreter start, thiz=%p, art_method=%p", thiz_, art_method);
            if (LIKELY(thiz_))
                SetEntryPointsToInterpreter(thiz_, art_method);
        }

    };
}
