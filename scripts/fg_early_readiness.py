"""Adapt OFXR V569's first-output/tail split to the pinned atlas derivative.

The early signal is explicitly supplied by the V4 module. Existing core
completion values continue to own every work slot, history and destination.
"""

from pathlib import Path


def replace(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) != count:
        raise ValueError(f"Early-readiness source context differs: {old[:100]!r}")
    return text.replace(old, new)


def prepare_early_readiness(output: Path, code: str) -> str:
    header_path = output / "xrfg/d3d12_frame_synthesizer.hpp"
    header = header_path.read_text(encoding="utf-8")
    header = replace(header, "    std::uint64_t synthetic_fence_value{};",
                     "    std::uint64_t synthetic_fence_value{};\n"
                     "    // Nonzero only when the caller's shared first-output signal was queued.\n"
                     "    std::uint64_t first_output_signal_value{};")
    header = replace(header, "        bool enable_shared_nvidia_3x = false) noexcept;",
                     "        bool enable_shared_nvidia_3x = false,\n"
                     "        bool enable_first_output_signal = false) noexcept;")
    header = replace(header, "        std::span<const D3D12ExtraSynthetic> extra_synthetics) noexcept;",
                     "        std::span<const D3D12ExtraSynthetic> extra_synthetics,\n"
                     "        ID3D12Fence* first_output_fence = nullptr,\n"
                     "        std::uint64_t first_output_value = 0) noexcept;")
    header_path.write_text(header, encoding="utf-8")

    code = replace(code, "        ComPtr<ID3D12GraphicsCommandList> synthesis_command_list;",
                   "        ComPtr<ID3D12GraphicsCommandList> synthesis_command_list;\n"
                   "        ComPtr<ID3D12CommandAllocator> tail_allocator;\n"
                   "        ComPtr<ID3D12GraphicsCommandList> tail_command_list;\n"
                   "        bool tail_recorded{};")
    code = replace(code, "    bool enable_shared_nvidia_3x{};",
                   "    bool enable_shared_nvidia_3x{};\n"
                   "    bool enable_first_output_signal{};\n"
                   "    ComPtr<ID3D12Fence> first_output_fence;\n"
                   "    std::uint64_t first_output_value{}, last_first_output_value{};")
    code = replace(code, "        bool input_enable_shared_nvidia_3x) {",
                   "        bool input_enable_shared_nvidia_3x,\n"
                   "        bool input_enable_first_output_signal) {")
    code = replace(code, "        enable_shared_nvidia_3x = input_enable_shared_nvidia_3x;",
                   "        enable_shared_nvidia_3x = input_enable_shared_nvidia_3x;\n"
                   "        enable_first_output_signal = input_enable_first_output_signal;")
    code = replace(code, "    bool enable_shared_nvidia_3x) noexcept {",
                   "    bool enable_shared_nvidia_3x,\n"
                   "    bool enable_first_output_signal) noexcept {")
    code = replace(code, "            enable_shared_nvidia_3x);",
                   "            enable_shared_nvidia_3x, enable_first_output_signal);")
    code = replace(code, "impl_->synthetic_output_layout, impl_->enable_shared_nvidia_3x);",
                   "impl_->synthetic_output_layout, impl_->enable_shared_nvidia_3x,\n"
                   "            impl_->enable_first_output_signal);", 2)

    # Allocate once at initialization, never during an ordinary submission.
    allocation = """            if (enable_first_output_signal) {
                result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(slot.tail_allocator.GetAddressOf()));
                if (FAILED(result)) return result;
                result = device->CreateCommandList(node_mask, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    slot.tail_allocator.Get(), nullptr, IID_PPV_ARGS(slot.tail_command_list.GetAddressOf()));
                if (FAILED(result)) return result;
                slot.tail_command_list->SetName(L"OFXR Later Synthetics");
                result = slot.tail_command_list->Close();
                if (FAILED(result)) return result;
            }
"""
    code = replace(code, "            // Stable DRED names let the host identify which bridge phase was executing when",
                   allocation + "            // Stable DRED names let the host identify which bridge phase was executing when")
    code = replace(code, "        slot.timing_metadata = {};\n        slot.timing_pending = false;",
                   "        slot.timing_metadata = {};\n        slot.timing_pending = false;\n"
                   "        slot.tail_recorded = false;\n        last_first_output_value = 0;\n"
                   "        if (slot.tail_allocator) {\n"
                   "            const HRESULT tail = slot.tail_allocator->Reset();\n"
                   "            if (FAILED(tail)) { synthesis_enabled = false; return tail; }\n"
                   "        }")

    helper = """    [[nodiscard]] HRESULT begin_tail_list(WorkSlot& slot,
        ID3D12GraphicsCommandList*& commands, ID3D12PipelineState* pipeline) noexcept {
        if (!first_output_fence || !first_output_value) return S_OK;
        if (!enable_first_output_signal || !slot.tail_command_list || slot.tail_recorded)
            return E_UNEXPECTED;
        const HRESULT result = slot.tail_command_list->Reset(slot.tail_allocator.Get(), pipeline);
        if (FAILED(result)) return result;
        slot.tail_recorded = true;
        commands = slot.tail_command_list.Get();
        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(root_signature.Get());
        const auto start = slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        commands->SetGraphicsRootDescriptorTable(0, start);
        commands->SetGraphicsRootDescriptorTable(1,
            offset_gpu_handle(start, kSrvDescriptorCount, descriptor_increment));
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        return S_OK;
    }

    [[nodiscard]] HRESULT signal_first_and_submit_tail(WorkSlot& slot) noexcept {
        if (!slot.tail_recorded) return S_OK;
        const HRESULT result = queue->Signal(first_output_fence.Get(), first_output_value);
        if (FAILED(result)) return result;
        last_first_output_value = first_output_value;
        ID3D12CommandList* tail[] = {slot.tail_command_list.Get()};
        queue->ExecuteCommandLists(1, tail);
        return S_OK;
    }

"""
    code = replace(code, "    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {",
                   helper + "    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {")

    start = code.index("    [[nodiscard]] HRESULT record_nvidia_pair(")
    end = code.index("    [[nodiscard]] HRESULT record_pair(", start)
    block = code[start:end]
    block = replace(block, "ID3D12GraphicsCommandList* const synthesis =",
                    "ID3D12GraphicsCommandList* synthesis =")
    block = replace(block, "            for (UINT output = 0; output < synthetic_output_count(); ++output) {",
                     "            for (UINT output = 0; output < synthetic_output_count(); ++output) {\n"
                     "                if (output == 1) {\n"
                     "                    const HRESULT tail = begin_tail_list(slot, synthesis,\n"
                     "                        nvidia_options.bidirectional ? nvidia_bidirectional_graphics_pipeline.Get()\n"
                     "                                                     : nvidia_graphics_pipeline.Get());\n"
                     "                    if (FAILED(tail)) return tail;\n"
                     "                }")
    code = code[:start] + block + code[end:]

    start = code.index("    [[nodiscard]] HRESULT record_pair(")
    end = code.index("    [[nodiscard]] HRESULT record_repeated_pair(", start)
    block = code[start:end]
    # The FidelityFX flow and S1 stay on the first list; later draws and final
    # resource/timestamp cleanup must follow the tail's new recording pointer.
    block = replace(block, "        if (nvidia_gpu_timing_enabled) {",
                     "        ID3D12GraphicsCommandList* commands = slot.command_list.Get();\n"
                     "        if (nvidia_gpu_timing_enabled) {")
    block = block.replace("slot.command_list.Get()", "commands").replace("slot.command_list->", "commands->")
    block = block.replace("ID3D12GraphicsCommandList* commands = commands;",
                          "ID3D12GraphicsCommandList* commands = slot.command_list.Get();")
    block = replace(block, "        for (UINT output = 0; output < synthetic_output_count(); ++output) {",
                     "        for (UINT output = 0; output < synthetic_output_count(); ++output) {\n"
                     "            if (output == 1) {\n"
                     "                const HRESULT tail = begin_tail_list(slot, commands, graphics_pipeline.Get());\n"
                     "                if (FAILED(tail)) return tail;\n"
                     "            }")
    code = code[:start] + block + code[end:]

    # Close every recorded list before queuing any part of a transaction.
    for name, next_name in [("execute_and_signal", "flush_pending_copy"),
                             ("execute_nvidia_and_signal", "submit_prime")]:
        start = code.index(f"    [[nodiscard]] HRESULT {name}(")
        end = code.index(f"    [[nodiscard]] HRESULT {next_name}(", start)
        block = code[start:end]
        old = "        result = slot.current_copy_command_list->Close();"
        block = replace(block, old,
                         "        if (slot.tail_recorded) {\n"
                         "            result = slot.tail_command_list->Close();\n"
                         "            if (FAILED(result)) { synthesis_enabled = false; return result; }\n"
                         "        }\n" + old)
        if name == "execute_and_signal":
            old = "        completion_unknown = true;"
        else:
            old = "        queue->ExecuteCommandLists(1, synthesis_lists);"
        after = """
        result = signal_first_and_submit_tail(slot);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(*newly_acquired);
            return result;
        }
"""
        block = replace(block, old, old + after)
        code = code[:start] + block + code[end:]

    # Only an actually split ordinary pair advertises the early signal.
    code = replace(code, "        output_ticket->previous_serial = previous_serial;",
                   "        output_ticket->first_output_signal_value = last_first_output_value;\n"
                   "        output_ticket->previous_serial = previous_serial;")
    start = code.index("HRESULT D3D12FrameSynthesizer::submit_pair_outputs(")
    end = code.index("HRESULT D3D12FrameSynthesizer::flush_current_copy(", start)
    block = code[start:end]
    block = replace(block, "    std::span<const D3D12ExtraSynthetic> extra_synthetics) noexcept {",
                     "    std::span<const D3D12ExtraSynthetic> extra_synthetics,\n"
                     "    ID3D12Fence* first_fence, std::uint64_t first_value) noexcept {")
    block = replace(block, "        return impl_->submit_pair(",
                     "        if ((first_fence == nullptr) != (first_value == 0) ||\n"
                     "            (first_fence && !impl_->enable_first_output_signal)) {\n"
                     "            if (ticket) *ticket = {};\n"
                     "            return E_INVALIDARG;\n"
                     "        }\n"
                     "        impl_->first_output_fence = first_fence;\n"
                     "        impl_->first_output_value = first_value;\n"
                     "        return impl_->submit_pair(")
    code = code[:start] + block + code[end:]
    return code
