#include "wasm_module_manager.h"

#include <iostream>
#include <fstream>
#include <sstream>

bool WasmModuleManager::loadModule(const std::string &module_path,
                                    const std::string &module_name) {
  // Read WASM file
  std::ifstream file(module_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    std::cerr << "Failed to open WASM module: " << module_path << std::endl;
    return false;
  }

  size_t file_size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> code(file_size);
  file.read(reinterpret_cast<char *>(code.data()), file_size);
  file.close();

  if (!file) {
    std::cerr << "Failed to read WASM module: " << module_path << std::endl;
    return false;
  }

  return loadModuleFromMemory(code.data(), code.size(), module_name);
}

bool WasmModuleManager::loadModuleFromMemory(const uint8_t *code, size_t code_size,
                                              const std::string &module_name) {
  if (modules_.find(module_name) != modules_.end()) {
    std::cerr << "Module already loaded: " << module_name << std::endl;
    return false;
  }

  try {
    std::cout << "Loading WASM module: " << module_name << " (" << code_size << " bytes)"
              << std::endl;

    // Create a Wasmtime VM instance.
#ifdef ENABLE_WASMTIME
    auto vm = proxy_wasm::createWasmtimeVm();
#else
    std::cerr << "No WASM runtime available (Wasmtime not enabled)" << std::endl;
    return false;
#endif

    // Attach integration (logging/error hooks).
    vm->integration() = std::make_unique<lswasm::LsWasmIntegration>();

    // Create the WasmBase with environment variables.
    auto wasm = std::make_shared<lswasm::LsWasm>(
        std::move(vm), envs_, module_name, /*vm_configuration=*/"", /*vm_key=*/module_name);

    // Load the WASM bytecode.
    std::string bytecode(reinterpret_cast<const char *>(code), code_size);
    if (!wasm->load(bytecode, /*allow_precompiled=*/false)) {
      std::cerr << "Failed to load WASM bytecode for module: " << module_name << std::endl;
      return false;
    }

    // Initialize the VM (registers ABI callbacks, links imports, runs _initialize).
    if (!wasm->initialize()) {
      std::cerr << "Failed to initialize WASM module: " << module_name << std::endl;
      return false;
    }

    // Create a plugin for this module.
    auto plugin = std::make_shared<proxy_wasm::PluginBase>(
        /*name=*/module_name,
        /*root_id=*/module_name,
        /*vm_id=*/module_name,
        /*engine=*/"wasmtime",
        /*plugin_configuration=*/"",
        /*fail_open=*/false,
        /*key=*/module_name);

    // Start the VM (calls proxy_on_vm_start) and create a root context.
    auto *root_context = wasm->start(plugin);
    if (!root_context) {
      std::cerr << "Failed to start WASM module: " << module_name << std::endl;
      return false;
    }

    // Configure the plugin (calls proxy_on_configure).
    if (!wasm->configure(root_context, plugin)) {
      std::cerr << "Failed to configure WASM module: " << module_name << std::endl;
      return false;
    }

    ModuleState state;
    state.wasm = wasm;
    state.plugin = plugin;
    modules_[module_name] = std::move(state);

    std::cout << "Module loaded and initialized successfully: " << module_name << std::endl;
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Failed to load module " << module_name << ": " << e.what() << std::endl;
    return false;
  }
}

bool WasmModuleManager::executeFilter(const std::string &module_name, uint32_t context_id,
                                       const std::string &phase) {
  auto it = modules_.find(module_name);
  if (it == modules_.end()) {
    std::cerr << "Module not found: " << module_name << std::endl;
    return false;
  }

  auto &state = it->second;
  auto &wasm = state.wasm;

  if (!wasm || wasm->isFailed()) {
    std::cerr << "Module VM not available or failed: " << module_name << std::endl;
    return false;
  }

  std::cout << "[WASM] Executing " << phase << " phase for module: " << module_name
            << " (context_id: " << context_id << ")" << std::endl;

  try {
    // For onRequestHeaders, we need to create a stream context and call the filter.
    if (phase == "onRequestHeaders") {
      // Create a new stream context for this request if needed.
      if (!state.stream_context || state.stream_context_id != context_id) {
        // Get the root context for this plugin.
        auto *root_ctx = wasm->getRootContext(state.plugin, false);
        if (!root_ctx) {
          std::cerr << "No root context for module: " << module_name << std::endl;
          return false;
        }

        // Create a stream context via proxy_on_context_create.
        auto *ctx = wasm->createContext(state.plugin);
        if (!ctx) {
          std::cerr << "Failed to create stream context for module: " << module_name << std::endl;
          return false;
        }
        ctx->onCreate();

        state.stream_context = dynamic_cast<lswasm::LsWasmContext *>(ctx);
        state.stream_context_id = context_id;

        if (state.stream_context) {
          state.stream_context->resetLocalResponse();
        }
      }

      if (state.stream_context) {
        state.stream_context->onRequestHeaders(0, true);
      }
    } else if (phase == "onRequestBody") {
      if (state.stream_context) {
        state.stream_context->onRequestBody(0, true);
      }
    } else if (phase == "onRequestTrailers") {
      if (state.stream_context) {
        state.stream_context->onRequestTrailers(0);
      }
    } else if (phase == "onResponseHeaders") {
      if (state.stream_context) {
        state.stream_context->onResponseHeaders(0, true);
      }
    } else if (phase == "onResponseBody") {
      if (state.stream_context) {
        state.stream_context->onResponseBody(0, true);
      }
    } else if (phase == "onResponseTrailers") {
      if (state.stream_context) {
        state.stream_context->onResponseTrailers(0);
      }
    } else if (phase == "onDone") {
      if (state.stream_context) {
        state.stream_context->onDone();
      }
    } else {
      std::cout << "[WASM] Unknown phase: " << phase << std::endl;
    }

    return true;

  } catch (const std::exception &e) {
    std::cerr << "[WASM] Error executing " << phase << " for " << module_name << ": " << e.what()
              << std::endl;
    return false;
  }
}

std::string WasmModuleManager::getLocalResponseBody(const std::string &module_name) const {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseBody();
  }
  return "";
}

uint32_t WasmModuleManager::getLocalResponseCode(const std::string &module_name) const {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseCode();
  }
  return 0;
}

bool WasmModuleManager::hasLocalResponse(const std::string &module_name) const {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->hasLocalResponse();
  }
  return false;
}

bool WasmModuleManager::unloadModule(const std::string &module_name) {
  auto it = modules_.find(module_name);
  if (it == modules_.end()) {
    std::cerr << "Module not found: " << module_name << std::endl;
    return false;
  }

  modules_.erase(it);
  std::cout << "Module unloaded: " << module_name << std::endl;
  return true;
}

std::vector<std::string> WasmModuleManager::getLoadedModules() const {
  std::vector<std::string> result;
  for (const auto &pair : modules_) {
    result.push_back(pair.first);
  }
  return result;
}

bool WasmModuleManager::hasModule(const std::string &module_name) const {
  return modules_.find(module_name) != modules_.end();
}
