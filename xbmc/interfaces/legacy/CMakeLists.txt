set(SOURCES AddonCallback.cpp
            AddonClass.cpp
            Addon.cpp
            AddonUtils.cpp
            CallbackFunction.cpp
            CallbackHandler.cpp
            Control.cpp
            Dialog.cpp
            DrmCryptoSession.cpp
            File.cpp
            InfoTagGame.cpp
            InfoTagMusic.cpp
            InfoTagPicture.cpp
            InfoTagRadioRDS.cpp
            InfoTagVideo.cpp
            Keyboard.cpp
            LanguageHook.cpp
            ListItem.cpp
            ModuleXbmc.cpp
            ModuleXbmcgui.cpp
            ModuleXbmcplugin.cpp
            ModuleXbmcvfs.cpp
            Monitor.cpp
            Player.cpp
            PlayList.cpp
            Settings.cpp
            String.cpp
            Window.cpp
            WindowDialog.cpp
            WindowDialogMixin.cpp
            WindowXML.cpp)

set(HEADERS Addon.h
            AddonCallback.h
            AddonClass.h
            AddonString.h
            AddonUtils.h
            Alternative.h
            aojsonrpc.h
            CallbackFunction.h
            CallbackHandler.h
            Control.h
            Dialog.h
            Dictionary.h
            DrmCryptoSession.h
            Exception.h
            File.h
            InfoTagGame.h
            InfoTagMusic.h
            InfoTagPicture.h
            InfoTagRadioRDS.h
            InfoTagVideo.h
            Keyboard.h
            LanguageHook.h
            List.h
            ListItem.h
            ModuleXbmc.h
            ModuleXbmcgui.h
            ModuleXbmcplugin.h
            ModuleXbmcvfs.h
            Monitor.h
            Player.h
            PlayList.h
            RenderCapture.h
            Settings.h
            Stat.h
            swighelper.h
            Tuple.h
            Window.h
            WindowDialog.h
            WindowDialogMixin.h
            WindowException.h
            WindowInterceptor.h
            WindowXML.h)

core_add_library(legacy_interface)

# The Python bindings convert arguments by trying types and catching what fails, so they
# need C++ exceptions, which the WASM build otherwise leaves out.
if(CORE_SYSTEM_NAME STREQUAL wasm)
  target_compile_options(legacy_interface PRIVATE "SHELL:-sDISABLE_EXCEPTION_CATCHING=0")
endif()
