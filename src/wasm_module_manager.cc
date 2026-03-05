/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2026  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/

#include "wasm_module_manager.h"

#include <iostream>
#include <fstream>
#include <sstream>

#include "include/proxy-wasm/bytecode_util.h"

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

    // ---------------------------------------------------------------------------
    // Pre-flight check: verify the module is a valid proxy-wasm filter.
    // This catches the common mistake of passing a plain WASI CLI program
    // (e.g. one compiled with _start) instead of a proxy-wasm module (which
    // must export proxy_abi_version_0_1_0, 0_2_0, or 0_2_1).
    // ---------------------------------------------------------------------------
    {
      std::string_view bytecode_view(reinterpret_cast<const char *>(code), code_size);
      proxy_wasm::AbiVersion abi = proxy_wasm::AbiVersion::Unknown;

      if (!proxy_wasm::BytecodeUtil::getAbiVersion(bytecode_view, abi)) {
        LOG_ERROR("Failed to parse WASM bytecode for module: " << module_name
                  << " — the file may be corrupt or not a valid WebAssembly module.");
        return false;
      }

      if (abi == proxy_wasm::AbiVersion::Unknown) {
        LOG_ERROR("Module '" << module_name << "' is not a proxy-wasm filter module.\n"
                  << "    The WASM binary does not export a proxy-wasm ABI version\n"
                  << "    (expected proxy_abi_version_0_1_0, 0_2_0, or 0_2_1).\n"
                  << "    This usually means the module is a plain WASI CLI program\n"
                  << "    (e.g. compiled with _start).  lswasm only supports modules\n"
                  << "    built with the proxy-wasm ABI.\n"
                  << "    To run a plain WASI program, use wasmtime directly:\n"
                  << "      wasmtime run <module.wasm>");
        return false;
      }

      const char *abi_str = "unknown";
      switch (abi) {
        case proxy_wasm::AbiVersion::ProxyWasm_0_1_0: abi_str = "0.1.0"; break;
        case proxy_wasm::AbiVersion::ProxyWasm_0_2_0: abi_str = "0.2.0"; break;
        case proxy_wasm::AbiVersion::ProxyWasm_0_2_1: abi_str = "0.2.1"; break;
        default: break;
      }
      LOG_INFO("Detected proxy-wasm ABI version " << abi_str << " for module: " << module_name);
    }

    // Create a VM instance for the configured runtime.
#if defined(WASM_RUNTIME_WASMTIME)
    std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createWasmtimeVm();
#elif defined(WASM_RUNTIME_V8)
    LOG_INFO("Creating V8VM");
    std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createV8Vm();
    LOG_INFO("Created V8VM");
#else
    LOG_ERROR("No WASM runtime available (build with -DWASM_RUNTIME=wasmtime or -DWASM_RUNTIME=v8)");
    return false;
#endif

    // Attach integration (logging/error hooks).
    vm->integration() = std::make_unique<lswasm::LsWasmIntegration>();

    // Create the WasmBase with environment variables.
    std::shared_ptr<lswasm::LsWasm> wasm = std::make_shared<lswasm::LsWasm>(
        std::move(vm), envs_, module_name, /*vm_configuration=*/"", /*vm_key=*/module_name);

    // Load the WASM bytecode.
    std::string bytecode(reinterpret_cast<const char *>(code), code_size);
    LOG_INFO("Loading bytecode");
    if (!wasm->load(bytecode, /*allow_precompiled=*/false)) {
      LOG_ERROR("Failed to load WASM bytecode for module: " << module_name);
      return false;
    }

    // Initialize the VM (registers ABI callbacks, links imports, runs _initialize).
    LOG_INFO("Initializing...");
    if (!wasm->initialize()) {
      LOG_ERROR("Failed to initialize WASM module: " << module_name);
      return false;
    }

    // Create a plugin for this module.
    // NOTE: root_id must match the root_id used in the SDK's RegisterContextFactory.
    // The proxy-wasm-cpp-sdk default is "" (empty string).  If the WASM module
    // registers its factories with a specific root_id, this should be made
    // configurable.  Using "" here matches the common SDK default.
    LOG_INFO("Making plugin");
    std::shared_ptr<proxy_wasm::PluginBase> plugin = std::make_shared<proxy_wasm::PluginBase>(
        /*name=*/module_name,
        /*root_id=*/"",
        /*vm_id=*/module_name,
#if defined(WASM_RUNTIME_WASMTIME)
        /*engine=*/"wasmtime",
#elif defined(WASM_RUNTIME_V8)
        /*engine=*/"v8",
#else
        /*engine=*/"",
#endif
        /*plugin_configuration=*/"",
        /*fail_open=*/false,
        /*key=*/module_name);

    // Start the VM (calls proxy_on_vm_start) and create a root context.
    LOG_INFO("Starting VM and creating root context...");
    proxy_wasm::ContextBase *root_context = wasm->start(plugin);
    if (!root_context) {
      LOG_ERROR("Failed to start WASM module: " << module_name);
      return false;
    }

    // Configure the plugin (calls proxy_on_configure).
    LOG_INFO("Configuring plugin...");
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

bool WasmModuleManager::ensureStreamContext(const std::string &module_name, uint32_t context_id) {
  std::map<std::string, ModuleState>::iterator it = modules_.find(module_name);
  if (it == modules_.end()) {
    LOG_ERROR("Module not found: " << module_name);
    return false;
  }

  ModuleState &state = it->second;
  std::shared_ptr<lswasm::LsWasm> &wasm = state.wasm;

  if (!wasm || wasm->isFailed()) {
    LOG_ERROR("Module VM not available or failed: " << module_name);
    return false;
  }

  // Already have a stream context for this context_id — nothing to do.
  if (state.stream_context && state.stream_context_id == context_id) {
    return true;
  }

  // Get the root context for this plugin.
  proxy_wasm::ContextBase *root_ctx = wasm->getRootContext(state.plugin, false);
  if (!root_ctx) {
    LOG_ERROR("No root context for module: " << module_name);
    return false;
  }

  // Create a stream context via proxy_on_context_create.
  // NOTE: LsWasm::createContext() uses the ContextBase(WasmBase*, PluginBase)
  // constructor which makes the new context look like a root context
  // (parent_context_ == this).  We must patch the parent to the real
  // root context BEFORE calling onCreate(), so that
  // proxy_on_context_create(stream_id, root_id) passes the correct
  // root_context_id to the in-VM SDK.
  proxy_wasm::ContextBase *ctx = wasm->createContext(state.plugin);
  if (!ctx) {
    LOG_ERROR("Failed to create stream context for module: " << module_name);
    return false;
  }

  lswasm::LsWasmContext *lswasm_ctx = dynamic_cast<lswasm::LsWasmContext *>(ctx);
  if (lswasm_ctx) {
    lswasm_ctx->setParentContext(root_ctx);
  }

  ctx->onCreate();

  state.stream_context = dynamic_cast<lswasm::LsWasmContext *>(ctx);
  state.stream_context_id = context_id;

  if (state.stream_context) {
    state.stream_context->resetLocalResponse();
    state.stream_context->resetHeaderMaps();
  }

  return true;
}

bool WasmModuleManager::executeFilter(const std::string &module_name, uint32_t context_id,
                                       const std::string &phase) {
  std::map<std::string, ModuleState>::iterator it = modules_.find(module_name);
  if (it == modules_.end()) {
    LOG_ERROR("Module not found: " << module_name);
    return false;
  }

  ModuleState &state = it->second;
  std::shared_ptr<lswasm::LsWasm> &wasm = state.wasm;

  if (!wasm || wasm->isFailed()) {
    LOG_ERROR("Module VM not available or failed: " << module_name);
    return false;
  }

  LOG_INFO("[WASM] Executing " << phase << " phase for module: " << module_name
            << " (context_id: " << context_id << ")");

  try {
    // Ensure the stream context exists for this request.
    if (!ensureStreamContext(module_name, context_id)) {
      return false;
    }

    if (phase == "onRequestHeaders") {
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
  std::map<std::string, ModuleState>::const_iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseBody();
  }
  return "";
}

uint32_t WasmModuleManager::getLocalResponseCode(const std::string &module_name) const {
  std::map<std::string, ModuleState>::const_iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseCode();
  }
  return 0;
}

bool WasmModuleManager::hasLocalResponse(const std::string &module_name) const {
  std::map<std::string, ModuleState>::const_iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->hasLocalResponse();
  }
  return false;
}

HeaderPairs WasmModuleManager::getLocalResponseHeaders(const std::string &module_name) const {
  std::map<std::string, ModuleState>::const_iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->localResponseHeaders();
  }
  return {};
}

void WasmModuleManager::setContextHeaders(const std::string &module_name,
                                           proxy_wasm::WasmHeaderMapType type,
                                           const HeaderPairs &pairs) {
  std::map<std::string, ModuleState>::iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    it->second.stream_context->setHeaderMap(type, pairs);
  }
}

HeaderPairs WasmModuleManager::getContextHeaders(const std::string &module_name,
                                                   proxy_wasm::WasmHeaderMapType type) const {
  std::map<std::string, ModuleState>::const_iterator it = modules_.find(module_name);
  if (it != modules_.end() && it->second.stream_context) {
    return it->second.stream_context->getHeaderMapOwned(type);
  }
  return {};
}

bool WasmModuleManager::unloadModule(const std::string &module_name) {
  std::map<std::string, ModuleState>::iterator it = modules_.find(module_name);
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
  for (const std::pair<const std::string, ModuleState> &pair : modules_) {
    result.push_back(pair.first);
  }
  return result;
}

bool WasmModuleManager::hasModule(const std::string &module_name) const {
  return modules_.find(module_name) != modules_.end();
}
