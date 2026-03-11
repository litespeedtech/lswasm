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
  {
    std::shared_lock<std::shared_mutex> rlock(modules_mutex_);
    if (modules_.find(module_name) != modules_.end()) {
      LOG_ERROR("Module already loaded: " << module_name);
      return false;
    }
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
#elif defined(WASM_RUNTIME_WASMEDGE)
        /*engine=*/"wasmedge",
#elif defined(WASM_RUNTIME_WAMR)
        /*engine=*/"wamr",
#else
        /*engine=*/"",
#endif
        /*plugin_configuration=*/"",
        /*fail_open=*/false,
        /*key=*/module_name);

    // Capture environment variables for clone factory.
    auto envs = envs_;

    // ---- Factory lambdas for the proxy-wasm thread-local cloning API ----

    // WasmHandleFactory: creates a new base WasmHandle (called by createWasm()
    // when no cached base exists for the given vm_key).
    proxy_wasm::WasmHandleFactory wasm_handle_factory =
        [envs](std::string_view vm_key) -> std::shared_ptr<proxy_wasm::WasmHandleBase> {
#if defined(WASM_RUNTIME_WASMTIME)
      std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createWasmtimeVm();
#elif defined(WASM_RUNTIME_V8)
      LOG_INFO("Creating V8VM");
      std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createV8Vm();
      LOG_INFO("Created V8VM");
#elif defined(WASM_RUNTIME_WASMEDGE)
      LOG_INFO("Creating WasmEdge VM");
      std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createWasmEdgeVm();
      LOG_INFO("Created WasmEdge VM");
#elif defined(WASM_RUNTIME_WAMR)
      LOG_INFO("Creating WAMR VM");
      std::unique_ptr<proxy_wasm::WasmVm> vm = proxy_wasm::createWamrVm();
      LOG_INFO("Created WAMR VM");
#else
      LOG_ERROR("No WASM runtime available");
      return nullptr;
#endif
      vm->integration() = std::make_unique<lswasm::LsWasmIntegration>();

      auto wasm = std::make_shared<lswasm::LsWasm>(
          std::move(vm), envs, vm_key, /*vm_configuration=*/"", vm_key);
      return std::make_shared<lswasm::LsWasmHandle>(std::move(wasm));
    };

    // WasmHandleCloneFactory: creates a thread-local VM clone from a base handle.
    // Each clone gets its own V8 Store/Isolate (via WasmVm::clone()) and shares
    // the MetricStore with the base VM.
    //
    // The LsWasm clone constructor delegates to WasmBase(base_handle, factory)
    // which internally calls WasmVm::clone(), then getOrCreateThreadLocalWasm()
    // calls load() + initialize() after the clone factory returns.
    proxy_wasm::WasmHandleCloneFactory clone_factory =
        [](std::shared_ptr<proxy_wasm::WasmHandleBase> base_handle)
            -> std::shared_ptr<proxy_wasm::WasmHandleBase> {
      auto cloned_wasm = std::make_shared<lswasm::LsWasm>(
          base_handle, /*factory=*/nullptr);
      return std::make_shared<lswasm::LsWasmHandle>(std::move(cloned_wasm));
    };

    // PluginHandleFactory: creates a PluginHandle wrapping a WasmHandle + Plugin.
    proxy_wasm::PluginHandleFactory plugin_factory =
        [](std::shared_ptr<proxy_wasm::WasmHandleBase> wasm_handle,
           std::shared_ptr<proxy_wasm::PluginBase> plugin_base)
            -> std::shared_ptr<proxy_wasm::PluginHandleBase> {
      return std::make_shared<lswasm::LsPluginHandle>(
          std::move(wasm_handle), std::move(plugin_base));
    };

    // Build the VM key for the base_wasms registry.
    std::string bytecode(reinterpret_cast<const char *>(code), code_size);
    std::string vm_key = proxy_wasm::makeVmKey(
        /*vm_id=*/module_name, /*configuration=*/"", bytecode);

    // Use createWasm() to create (or retrieve cached) base VM handle.
    // This also runs load(), initialize(), start(), configure(), and canary.
    LOG_INFO("Creating base WASM handle via createWasm()...");
    auto base_handle = proxy_wasm::createWasm(
        vm_key, bytecode, plugin, wasm_handle_factory, clone_factory,
        /*allow_precompiled=*/false);
    if (!base_handle) {
      LOG_ERROR("Failed to create base WASM handle for module: " << module_name);
      return false;
    }

    // Verify the base handle contains an LsWasm instance.
    auto base_lswasm = std::dynamic_pointer_cast<lswasm::LsWasm>(base_handle->wasm());
    if (!base_lswasm) {
      LOG_ERROR("Base handle does not contain an LsWasm instance for module: " << module_name);
      return false;
    }

    // Start the VM (calls proxy_on_vm_start) and create a root context on the base VM.
    LOG_INFO("Starting VM and creating root context...");
    proxy_wasm::ContextBase *root_context = base_lswasm->start(plugin);
    if (!root_context) {
      LOG_ERROR("Failed to start WASM module: " << module_name);
      return false;
    }

    // Configure the plugin (calls proxy_on_configure).
    LOG_INFO("Configuring plugin...");
    if (!base_lswasm->configure(root_context, plugin)) {
      LOG_ERROR("Failed to configure WASM module: " << module_name);
      return false;
    }

    ModuleState state;
    state.base_handle = std::move(base_handle);
    state.plugin = std::move(plugin);
    state.clone_factory = std::move(clone_factory);
    state.plugin_factory = std::move(plugin_factory);

    {
      std::unique_lock<std::shared_mutex> wlock(modules_mutex_);
      modules_[module_name] = std::move(state);
    }

    LOG_INFO("Module loaded and initialized successfully: " << module_name);
    return true;

  } catch (const std::exception &e) {
    LOG_ERROR("Failed to load module " << module_name << ": " << e.what());
    return false;
  }
}

bool WasmModuleManager::createRequestScope(const std::string &module_name,
                                            uint32_t context_id,
                                            RequestScope &scope) const {
  std::shared_lock<std::shared_mutex> rlock(modules_mutex_);
  auto it = modules_.find(module_name);
  if (it == modules_.end()) {
    LOG_ERROR("Module not found: " << module_name);
    return false;
  }

  return scope.init(it->second, context_id);
}

bool WasmModuleManager::unloadModule(const std::string &module_name) {
  std::unique_lock<std::shared_mutex> wlock(modules_mutex_);
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
  std::shared_lock<std::shared_mutex> rlock(modules_mutex_);
  std::vector<std::string> result;
  for (const auto &[name, _state] : modules_) {
    result.push_back(name);
  }
  return result;
}

bool WasmModuleManager::hasModule(const std::string &module_name) const {
  std::shared_lock<std::shared_mutex> rlock(modules_mutex_);
  return modules_.find(module_name) != modules_.end();
}
