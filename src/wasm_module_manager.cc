#include "wasm_module_manager.h"

#include <iostream>
#include <fstream>
#include <sstream>

bool WasmModuleManager::loadModule(const std::string &module_path,
                                    const std::string &module_name) {
  // Read WASM file
  std::ifstream file(module_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERROR("Failed to open WASM module: " << module_path);
    return false;
  }

  size_t file_size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> code(file_size);
  file.read(reinterpret_cast<char *>(code.data()), file_size);
  file.close();

  if (!file) {
    LOG_ERROR("Failed to read WASM module: " << module_path);
    return false;
  }

  return loadModuleFromMemory(code.data(), code.size(), module_name);
}

bool WasmModuleManager::loadModuleFromMemory(const uint8_t *code, size_t code_size,
                                              const std::string &module_name) {
  if (modules_.find(module_name) != modules_.end()) {
    LOG_ERROR("Module already loaded: " << module_name);
    return false;
  }

  try {
    LOG_INFO("Loading WASM module: " << module_name << " (" << code_size << " bytes)");

    // Create a VM instance for the configured runtime.
#if defined(WASM_RUNTIME_WASMTIME)
    auto vm = proxy_wasm::createWasmtimeVm();
#elif defined(WASM_RUNTIME_V8)
    auto vm = proxy_wasm::createV8Vm();
#else
    LOG_ERROR("No WASM runtime available (build with -DWASM_RUNTIME=wasmtime or -DWASM_RUNTIME=v8)");
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
      LOG_ERROR("Failed to load WASM bytecode for module: " << module_name);
      return false;
    }

    // Initialize the VM (registers ABI callbacks, links imports, runs _initialize).
    if (!wasm->initialize()) {
      LOG_ERROR("Failed to initialize WASM module: " << module_name);
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
      LOG_ERROR("Failed to start WASM module: " << module_name);
      return false;
    }

    // Configure the plugin (calls proxy_on_configure).
    if (!wasm->configure(root_context, plugin)) {
      LOG_ERROR("Failed to configure WASM module: " << module_name);
      return false;
    }

    ModuleState state;
    state.wasm = wasm;
    state.plugin = plugin;
    modules_[module_name] = std::move(state);

    LOG_INFO("Module loaded and initialized successfully: " << module_name);
    return true;

  } catch (const std::exception &e) {
    LOG_ERROR("Failed to load module " << module_name << ": " << e.what());
    return false;
  }
}

bool WasmModuleManager::executeFilter(const std::string &module_name, uint32_t context_id,
                                       const std::string &phase) {
  auto it = modules_.find(module_name);
  if (it == modules_.end()) {
    LOG_ERROR("Module not found: " << module_name);
    return false;
  }

  auto &state = it->second;
  auto &wasm = state.wasm;

  if (!wasm || wasm->isFailed()) {
    LOG_ERROR("Module VM not available or failed: " << module_name);
    return false;
  }

  LOG_INFO("[WASM] Executing " << phase << " phase for module: " << module_name
            << " (context_id: " << context_id << ")");

  try {
    // For onRequestHeaders, we need to create a stream context and call the filter.
    if (phase == "onRequestHeaders") {
      // Create a new stream context for this request if needed.
      if (!state.stream_context || state.stream_context_id != context_id) {
        // Get the root context for this plugin.
        auto *root_ctx = wasm->getRootContext(state.plugin, false);
        if (!root_ctx) {
          LOG_ERROR("No root context for module: " << module_name);
          return false;
        }

        // Create a stream context via proxy_on_context_create.
        auto *ctx = wasm->createContext(state.plugin);
        if (!ctx) {
          LOG_ERROR("Failed to create stream context for module: " << module_name);
          return false;
        }
        ctx->onCreate();

        state.stream_context = dynamic_cast<lswasm::LsWasmContext *>(ctx);
        state.stream_context_id = context_id;

        if (state.stream_context) {
          state.stream_context->resetLocalResponse();
          state.stream_context->resetHeaderMaps();
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
      LOG_INFO("[WASM] Unknown phase: " << phase);
    }

    return true;

  } catch (const std::exception &e) {
    LOG_ERROR("[WASM] Error executing " << phase << " for " << module_name << ": " << e.what());
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

HeaderPairs WasmModuleManager::getLocalResponseHeaders(const std::string &module_name) const {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseHeaders();
  }
  return {};
}

void WasmModuleManager::setContextHeaders(const std::string &module_name,
                                           proxy_wasm::WasmHeaderMapType type,
                                           const HeaderPairs &pairs) {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    it->second.stream_context->setHeaderMap(type, pairs);
  }
}

HeaderPairs WasmModuleManager::getContextHeaders(const std::string &module_name,
                                                  proxy_wasm::WasmHeaderMapType type) const {
  auto it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->getHeaderMapOwned(type);
  }
  return {};
}

bool WasmModuleManager::unloadModule(const std::string &module_name) {
  auto it = modules_.find(module_name);
  if (it == modules_.end()) {
    LOG_ERROR("Module not found: " << module_name);
    return false;
  }

  modules_.erase(it);
  LOG_INFO("Module unloaded: " << module_name);
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
