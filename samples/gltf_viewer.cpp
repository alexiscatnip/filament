/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define GLTFIO_SIMPLEVIEWER_IMPLEMENTATION

#include "app/Config.h"
#include "app/FilamentApp.h"
#include "app/IBL.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/SimpleViewer.h>

#include <getopt/getopt.h>

#include <utils/NameComponentManager.h>

#include <fstream>
#include <string>

#include "generated/resources/gltf.h"

using namespace filament;
using namespace gltfio;
using namespace utils;

struct App {
    SimpleViewer* viewer;
    Config config;
    AssetLoader* loader;
    FilamentAsset* asset;
    NameComponentManager* names;
    MaterialSource materialSource = GENERATE_SHADERS;
};

static const char* DEFAULT_IBL = "envs/venetian_crossroads";

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
        "SHOWCASE renders the specified glTF file, or a built-in file if none is specified\n"
        "Usage:\n"
        "    SHOWCASE [options] <gltf file>\n"
        "Options:\n"
        "   --help, -h\n"
        "       Prints this message\n\n"
        "   --api, -a\n"
        "       Specify the backend API: opengl (default), vulkan, or metal\n\n"
        "   --ibl=<path to cmgen IBL>, -i <path>\n"
        "       Override the built-in IBL\n\n"
        "   --ubershader, -u\n"
        "       Enable ubershaders (improves load time, adds shader complexity)\n\n"
    );
    const std::string from("SHOWCASE");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    std::cout << usage;
}

static int handleCommandLineArguments(int argc, char* argv[], App* app) {
    static constexpr const char* OPTSTR = "ha:i:u";
    static const struct option OPTIONS[] = {
        { "help",       no_argument,       nullptr, 'h' },
        { "api",        required_argument, nullptr, 'a' },
        { "ibl",        required_argument, nullptr, 'i' },
        { "ubershader", no_argument,       nullptr, 'u' },
        { nullptr, 0, nullptr, 0 }
    };
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'a':
                if (arg == "opengl") {
                    app->config.backend = Engine::Backend::OPENGL;
                } else if (arg == "vulkan") {
                    app->config.backend = Engine::Backend::VULKAN;
                } else if (arg == "metal") {
                    app->config.backend = Engine::Backend::METAL;
                } else {
                    std::cerr << "Unrecognized backend. Must be 'opengl'|'vulkan'|'metal'.\n";
                }
                break;
            case 'i':
                app->config.iblDirectory = arg;
                break;
            case 'u':
                app->materialSource = LOAD_UBERSHADERS;
                break;
        }
    }
    return optind;
}

static std::ifstream::pos_type getFileSize(const char* filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}

int main(int argc, char** argv) {
    App app;

    app.config.title = "Filament";
    app.config.iblDirectory = FilamentApp::getRootPath() + DEFAULT_IBL;

    int option_index = handleCommandLineArguments(argc, argv, &app);
    utils::Path filename;
    int num_args = argc - option_index;
    if (num_args >= 1) {
        filename = argv[option_index];
        if (!filename.exists()) {
            std::cerr << "file " << argv[option_index] << " not found!" << std::endl;
            return 1;
        }
    }

    auto setup = [&app, filename](Engine* engine, View* view, Scene* scene) {
        app.names = new NameComponentManager(EntityManager::get());
        app.viewer = new SimpleViewer(engine, scene, view);
        app.loader = AssetLoader::create({engine, app.names, app.materialSource});
        if (filename.isEmpty()) {
            app.asset = app.loader->createAssetFromBinary(GLTF_DAMAGEDHELMET_DATA,
                    GLTF_DAMAGEDHELMET_SIZE);
        } else {
            // Peek at the file size to allow pre-allocation.
            long contentSize = static_cast<long>(getFileSize(filename.c_str()));
            if (contentSize <= 0) {
                std::cerr << "Unable to open " << filename << std::endl;
                exit(1);
            }

            // Consume the glTF file.
            std::ifstream in(filename.c_str(), std::ifstream::in);
            std::vector<uint8_t> buffer(static_cast<unsigned long>(contentSize));
            if (!in.read((char*) buffer.data(), contentSize)) {
                std::cerr << "Unable to read " << filename << std::endl;
                exit(1);
            }

            // Parse the glTF file and create Filament entities.
            if (filename.getExtension() == "glb") {
                app.asset = app.loader->createAssetFromBinary(buffer.data(), buffer.size());
            } else {
                app.asset = app.loader->createAssetFromJson(buffer.data(), buffer.size());
            }
            buffer.clear();
            buffer.shrink_to_fit();
        }
        if (!app.asset) {
            std::cerr << "Unable to parse " << filename << std::endl;
            exit(1);
        }

        // Load external textures and buffers.
        utils::Path assetFolder = filename.getParent();
        gltfio::ResourceLoader({engine, assetFolder, true, false}).loadResources(app.asset);

        // Load animation data then free the source hierarchy.
        app.asset->getAnimator();
        app.asset->releaseSourceData();

        // Add the renderables to the scene.
        app.viewer->setAsset(app.asset, app.names);

        // Leave FXAA enabled but we also enable MSAA for a nice result. The wireframe looks
        // much better with MSAA enabled.
        view->setSampleCount(4);
    };

    auto cleanup = [&app](Engine* engine, View*, Scene*) {
        Fence::waitAndDestroy(engine->createFence());
        delete app.viewer;
        app.loader->destroyAsset(app.asset);
        app.loader->destroyMaterials();
        AssetLoader::destroy(&app.loader);
        delete app.names;
    };

    auto animate = [&app](Engine* engine, View* view, double now) {
        app.viewer->applyAnimation(now);
    };

    auto gui = [&app](filament::Engine* engine, filament::View* view) {
        app.viewer->updateUserInterface();
        FilamentApp::get().setSidebarWidth(app.viewer->getSidebarWidth());
    };

    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.animate(animate);
    filamentApp.run(app.config, setup, cleanup, gui);

    return 0;
}
