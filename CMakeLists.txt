cmake_minimum_required(VERSION 3.22)

# Internal target name stays "VocalDAW" so the existing build folder keeps working;
# the app itself is called AMETHYST (AMETHYST.exe).
# macOS: support macOS 13 Ventura and later, one universal binary for Apple Silicon + Intel.
# (Must be set before project(); ignored on Windows.)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")

project(VocalDAW VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(VocalDAW
    PRODUCT_NAME "AMETHYST"
    COMPANY_NAME "AMETHYST"
    VERSION      ${PROJECT_VERSION}
    # macOS bundle settings (ignored on Windows)
    BUNDLE_ID                      com.amethyst.daw
    MICROPHONE_PERMISSION_ENABLED  TRUE
    MICROPHONE_PERMISSION_TEXT     "AMETHYST records your vocals from your microphone or audio interface."
    PLIST_TO_MERGE [[
        <plist>
          <dict>
            <key>NSDocumentsFolderUsageDescription</key>
            <string>AMETHYST saves your takes, projects and presets in Documents/AMETHYST.</string>
            <key>LSApplicationCategoryType</key>
            <string>public.app-category.music</string>
            <key>NSHighResolutionCapable</key>
            <true/>
          </dict>
        </plist>
    ]]
)

juce_generate_juce_header(VocalDAW)

target_sources(VocalDAW PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/Presets.cpp
    Source/GlassLookAndFeel.cpp
    Source/ClipFx.cpp
    Source/ClipEditor.cpp
    Source/SystemInfo.cpp
    Source/UiHelpers.cpp
    Source/Mixer.cpp
    Source/TrackComponent.cpp
    Source/MainComponent.cpp
    Source/Plugins/AmethystPlugin.cpp
    Source/Plugins/EqPlugin.cpp
    Source/Plugins/DynamicsReverb.cpp
    Source/Plugins/TimeEffects.cpp
    Source/Plugins/VocalEffects.cpp
    Source/Plugins/TunePlugin.cpp
    Source/Account.cpp
    Source/AccountUI.cpp
    Source/Exporter.cpp
    Source/Encoders.cpp
)

# --- Supabase (accounts / AMETHYST Pro) ---------------------------------------
# Put your project URL and anon (publishable) key in config/supabase.json
# (gitignored; see config/supabase.example.json), or pass -DAMETHYST_SUPABASE_URL=... etc.
set(AMETHYST_SUPABASE_URL "" CACHE STRING "Supabase project URL")
set(AMETHYST_SUPABASE_ANON_KEY "" CACHE STRING "Supabase anon / publishable key")

set(_sb_config "${CMAKE_CURRENT_SOURCE_DIR}/config/supabase.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sb_config}")
set(_sb_url "${AMETHYST_SUPABASE_URL}")
set(_sb_key "${AMETHYST_SUPABASE_ANON_KEY}")
if(EXISTS "${_sb_config}")
    file(READ "${_sb_config}" _sb_json)
    string(JSON _json_url ERROR_VARIABLE _e1 GET "${_sb_json}" url)
    string(JSON _json_key ERROR_VARIABLE _e2 GET "${_sb_json}" anonKey)
    if(NOT _e1 AND _json_url)
        set(_sb_url "${_json_url}")
    endif()
    if(NOT _e2 AND _json_key)
        set(_sb_key "${_json_key}")
    endif()
endif()
if(_sb_url STREQUAL "" OR _sb_key STREQUAL "")
    message(STATUS "AMETHYST: Supabase not configured - sign-in disabled, Pro effects locked")
else()
    message(STATUS "AMETHYST: Supabase configured (${_sb_url})")
endif()
target_compile_definitions(VocalDAW PRIVATE
    AMETHYST_SUPABASE_URL="${_sb_url}"
    AMETHYST_SUPABASE_ANON_KEY="${_sb_key}"
)

target_compile_definitions(VocalDAW PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>             # Audio Units on macOS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=$<IF:$<PLATFORM_ID:Windows>,1,0>
)

if(APPLE)
    # Keychain (saved sign-in) + memory meter
    target_link_libraries(VocalDAW PRIVATE "-framework Security" "-framework CoreFoundation" "-framework AudioToolbox")
endif()

target_link_libraries(VocalDAW
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
