// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <tuple>
#include <utility>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
struct AttrInfo {
    Id pointer;
    Id id;
    bool needs_cast;
};

std::optional<AttrInfo> AttrTypes(EmitContext& ctx, u32 index) {
    const AttributeType type{ctx.runtime_info.generic_input_types.at(index)};
    switch (type) {
    case AttributeType::Float:
        return AttrInfo{ctx.input_f32, ctx.F32[1], false};
    case AttributeType::UnsignedInt:
        return AttrInfo{ctx.input_u32, ctx.U32[1], true};
    case AttributeType::SignedInt:
        return AttrInfo{ctx.input_s32, ctx.TypeInt(32, true), true};
    case AttributeType::Disabled:
        return std::nullopt;
    }
    throw InvalidArgument("Invalid attribute type {}", type);
}

template <typename... Args>
Id AttrPointer(EmitContext& ctx, Id pointer_type, Id vertex, Id base, Args&&... args) {
    switch (ctx.stage) {
    case Stage::TessellationControl:
    case Stage::TessellationEval:
    case Stage::Geometry:
        return ctx.OpAccessChain(pointer_type, base, vertex, std::forward<Args>(args)...);
    default:
        return ctx.OpAccessChain(pointer_type, base, std::forward<Args>(args)...);
    }
}

template <typename... Args>
Id OutputAccessChain(EmitContext& ctx, Id result_type, Id base, Args&&... args) {
    if (ctx.stage == Stage::TessellationControl) {
        const Id invocation_id{ctx.OpLoad(ctx.U32[1], ctx.invocation_id)};
        return ctx.OpAccessChain(result_type, base, invocation_id, std::forward<Args>(args)...);
    } else {
        return ctx.OpAccessChain(result_type, base, std::forward<Args>(args)...);
    }
}

struct OutAttr {
    OutAttr(Id pointer_) : pointer{pointer_} {}
    OutAttr(Id pointer_, Id type_) : pointer{pointer_}, type{type_} {}

    Id pointer{};
    Id type{};
};

std::optional<OutAttr> OutputAttrPointer(EmitContext& ctx, IR::Attribute attr) {
    if (IR::IsGeneric(attr)) {
        const u32 index{IR::GenericAttributeIndex(attr)};
        const u32 element{IR::GenericAttributeElement(attr)};
        const GenericElementInfo& info{ctx.output_generics.at(index).at(element)};
        if (info.num_components == 1) {
            return info.id;
        } else {
            const u32 index_element{element - info.first_element};
            const Id index_id{ctx.Const(index_element)};
            return OutputAccessChain(ctx, ctx.output_f32, info.id, index_id);
        }
    }
    switch (attr) {
    case IR::Attribute::PointSize:
        return ctx.output_point_size;
    case IR::Attribute::PositionX:
    case IR::Attribute::PositionY:
    case IR::Attribute::PositionZ:
    case IR::Attribute::PositionW: {
        const u32 element{static_cast<u32>(attr) % 4};
        const Id element_id{ctx.Const(element)};
        return OutputAccessChain(ctx, ctx.output_f32, ctx.output_position, element_id);
    }
    case IR::Attribute::ClipDistance0:
    case IR::Attribute::ClipDistance1:
    case IR::Attribute::ClipDistance2:
    case IR::Attribute::ClipDistance3:
    case IR::Attribute::ClipDistance4:
    case IR::Attribute::ClipDistance5:
    case IR::Attribute::ClipDistance6:
    case IR::Attribute::ClipDistance7: {
        const u32 base{static_cast<u32>(IR::Attribute::ClipDistance0)};
        const u32 index{static_cast<u32>(attr) - base};
        const Id clip_num{ctx.Const(index)};
        return OutputAccessChain(ctx, ctx.output_f32, ctx.clip_distances, clip_num);
    }
    case IR::Attribute::Layer:
        if (ctx.profile.support_viewport_index_layer_non_geometry ||
            ctx.stage == Shader::Stage::Geometry) {
            return OutAttr{ctx.layer, ctx.U32[1]};
        }
        return std::nullopt;
    case IR::Attribute::ViewportIndex:
        if (ctx.profile.support_viewport_index_layer_non_geometry ||
            ctx.stage == Shader::Stage::Geometry) {
            return OutAttr{ctx.viewport_index, ctx.U32[1]};
        }
        return std::nullopt;
    case IR::Attribute::ViewportMask:
        if (!ctx.profile.support_viewport_mask) {
            return std::nullopt;
        }
        return OutAttr{ctx.OpAccessChain(ctx.output_u32, ctx.viewport_mask, ctx.u32_zero_value),
                       ctx.U32[1]};
    default:
        throw NotImplementedException("Read attribute {}", attr);
    }
}

Id GetCbuf(EmitContext& ctx, Id result_type, Id UniformDefinitions::*member_ptr, u32 element_size,
           const IR::Value& binding, const IR::Value& offset, const Id indirect_func) {
    Id buffer_offset;
    const Id uniform_type{ctx.uniform_types.*member_ptr};
    if (offset.IsImmediate()) {
        // Hardware been proved to read the aligned offset (e.g. LDC.U32 at 6 will read offset 4)
        const Id imm_offset{ctx.Const(offset.U32() / element_size)};
        buffer_offset = imm_offset;
    } else if (element_size > 1) {
        const u32 log2_element_size{static_cast<u32>(std::countr_zero(element_size))};
        const Id shift{ctx.Const(log2_element_size)};
        buffer_offset = ctx.OpShiftRightArithmetic(ctx.U32[1], ctx.Def(offset), shift);
    } else {
        buffer_offset = ctx.Def(offset);
    }
    if (!binding.IsImmediate()) {
        return ctx.OpFunctionCall(result_type, indirect_func, ctx.Def(binding), buffer_offset);
    }
    const Id cbuf{ctx.cbufs[binding.U32()].*member_ptr};
    const Id access_chain{ctx.OpAccessChain(uniform_type, cbuf, ctx.u32_zero_value, buffer_offset)};
    return ctx.OpLoad(result_type, access_chain);
}

Id GetCbufU32(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    return GetCbuf(ctx, ctx.U32[1], &UniformDefinitions::U32, sizeof(u32), binding, offset,
                   ctx.load_const_func_u32);
}

Id GetCbufU32x4(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    return GetCbuf(ctx, ctx.U32[4], &UniformDefinitions::U32x4, sizeof(u32[4]), binding, offset,
                   ctx.load_const_func_u32x4);
}

Id GetCbufElement(EmitContext& ctx, Id vector, const IR::Value& offset, u32 index_offset) {
    if (offset.IsImmediate()) {
        const u32 element{(offset.U32() / 4) % 4 + index_offset};
        return ctx.OpCompositeExtract(ctx.U32[1], vector, element);
    }
    const Id shift{ctx.OpShiftRightArithmetic(ctx.U32[1], ctx.Def(offset), ctx.Const(2u))};
    Id element{ctx.OpBitwiseAnd(ctx.U32[1], shift, ctx.Const(3u))};
    if (index_offset > 0) {
        element = ctx.OpIAdd(ctx.U32[1], element, ctx.Const(index_offset));
    }
    return ctx.OpVectorExtractDynamic(ctx.U32[1], vector, element);
}
} // Anonymous namespace

void EmitGetRegister(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitSetRegister(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitGetPred(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitSetPred(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitSetGotoVariable(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitGetGotoVariable(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitSetIndirectBranchVariable(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

void EmitGetIndirectBranchVariable(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitGetCbufU8(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing && ctx.profile.support_int8) {
        const Id load{GetCbuf(ctx, ctx.U8, &UniformDefinitions::U8, sizeof(u8), binding, offset,
                              ctx.load_const_func_u8)};
        return ctx.OpUConvert(ctx.U32[1], load);
    }
    Id element{};
    if (ctx.profile.support_descriptor_aliasing) {
        element = GetCbufU32(ctx, binding, offset);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        element = GetCbufElement(ctx, vector, offset, 0u);
    }
    const Id bit_offset{ctx.BitOffset8(offset)};
    return ctx.OpBitFieldUExtract(ctx.U32[1], element, bit_offset, ctx.Const(8u));
}

Id EmitGetCbufS8(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing && ctx.profile.support_int8) {
        const Id load{GetCbuf(ctx, ctx.S8, &UniformDefinitions::S8, sizeof(s8), binding, offset,
                              ctx.load_const_func_u8)};
        return ctx.OpSConvert(ctx.U32[1], load);
    }
    Id element{};
    if (ctx.profile.support_descriptor_aliasing) {
        element = GetCbufU32(ctx, binding, offset);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        element = GetCbufElement(ctx, vector, offset, 0u);
    }
    const Id bit_offset{ctx.BitOffset8(offset)};
    return ctx.OpBitFieldSExtract(ctx.U32[1], element, bit_offset, ctx.Const(8u));
}

Id EmitGetCbufU16(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing && ctx.profile.support_int16) {
        const Id load{GetCbuf(ctx, ctx.U16, &UniformDefinitions::U16, sizeof(u16), binding, offset,
                              ctx.load_const_func_u16)};
        return ctx.OpUConvert(ctx.U32[1], load);
    }
    Id element{};
    if (ctx.profile.support_descriptor_aliasing) {
        element = GetCbufU32(ctx, binding, offset);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        element = GetCbufElement(ctx, vector, offset, 0u);
    }
    const Id bit_offset{ctx.BitOffset16(offset)};
    return ctx.OpBitFieldUExtract(ctx.U32[1], element, bit_offset, ctx.Const(16u));
}

Id EmitGetCbufS16(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing && ctx.profile.support_int16) {
        const Id load{GetCbuf(ctx, ctx.S16, &UniformDefinitions::S16, sizeof(s16), binding, offset,
                              ctx.load_const_func_u16)};
        return ctx.OpSConvert(ctx.U32[1], load);
    }
    Id element{};
    if (ctx.profile.support_descriptor_aliasing) {
        element = GetCbufU32(ctx, binding, offset);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        element = GetCbufElement(ctx, vector, offset, 0u);
    }
    const Id bit_offset{ctx.BitOffset16(offset)};
    return ctx.OpBitFieldSExtract(ctx.U32[1], element, bit_offset, ctx.Const(16u));
}

Id EmitGetCbufU32(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing) {
        return GetCbufU32(ctx, binding, offset);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        return GetCbufElement(ctx, vector, offset, 0u);
    }
}

Id EmitGetCbufF32(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing) {
        return GetCbuf(ctx, ctx.F32[1], &UniformDefinitions::F32, sizeof(f32), binding, offset,
                       ctx.load_const_func_f32);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        return ctx.OpBitcast(ctx.F32[1], GetCbufElement(ctx, vector, offset, 0u));
    }
}

Id EmitGetCbufU32x2(EmitContext& ctx, const IR::Value& binding, const IR::Value& offset) {
    if (ctx.profile.support_descriptor_aliasing) {
        return GetCbuf(ctx, ctx.U32[2], &UniformDefinitions::U32x2, sizeof(u32[2]), binding, offset,
                       ctx.load_const_func_u32x2);
    } else {
        const Id vector{GetCbufU32x4(ctx, binding, offset)};
        return ctx.OpCompositeConstruct(ctx.U32[2], GetCbufElement(ctx, vector, offset, 0u),
                                        GetCbufElement(ctx, vector, offset, 1u));
    }
}

Id EmitGetAttribute(EmitContext& ctx, IR::Attribute attr, Id vertex) {
    const u32 element{static_cast<u32>(attr) % 4};
    if (IR::IsGeneric(attr)) {
        const u32 index{IR::GenericAttributeIndex(attr)};
        const std::optional<AttrInfo> type{AttrTypes(ctx, index)};
        if (!type || !ctx.runtime_info.previous_stage_stores.Generic(index, element)) {
            // Attribute is disabled or varying component is not written
            return ctx.Const(element == 3 ? 1.0f : 0.0f);
        }
        const Id generic_id{ctx.input_generics.at(index)};
        const Id pointer{AttrPointer(ctx, type->pointer, vertex, generic_id, ctx.Const(element))};
        const Id value{ctx.OpLoad(type->id, pointer)};
        return type->needs_cast ? ctx.OpBitcast(ctx.F32[1], value) : value;
    }
    switch (attr) {
    case IR::Attribute::PrimitiveId:
        return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.primitive_id));
    case IR::Attribute::Layer:
        return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.layer));
    case IR::Attribute::PositionX:
    case IR::Attribute::PositionY:
    case IR::Attribute::PositionZ:
    case IR::Attribute::PositionW:
        return ctx.OpLoad(
            ctx.F32[1],
            ctx.need_input_position_indirect
                ? AttrPointer(ctx, ctx.input_f32, vertex, ctx.input_position, ctx.u32_zero_value,
                              ctx.Const(element))
                : AttrPointer(ctx, ctx.input_f32, vertex, ctx.input_position, ctx.Const(element)));
    case IR::Attribute::InstanceId:
        if (ctx.profile.support_vertex_instance_id) {
            return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.instance_id));
        } else {
            const Id index{ctx.OpLoad(ctx.U32[1], ctx.instance_index)};
            const Id base{ctx.OpLoad(ctx.U32[1], ctx.base_instance)};
            return ctx.OpBitcast(ctx.F32[1], ctx.OpISub(ctx.U32[1], index, base));
        }
    case IR::Attribute::VertexId:
        if (ctx.profile.support_vertex_instance_id) {
            return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.vertex_id));
        } else {
            // const Id index{ctx.OpLoad(ctx.U32[1], ctx.vertex_index)};
            // const Id base{ctx.OpLoad(ctx.U32[1], ctx.base_vertex)};
            return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.vertex_index));
        }
    case IR::Attribute::BaseInstance:
        return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.base_instance));
    case IR::Attribute::BaseVertex:
        return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.base_vertex));
    case IR::Attribute::DrawID:
        return ctx.OpBitcast(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.draw_index));
    case IR::Attribute::FrontFace:
        return ctx.OpSelect(ctx.F32[1], ctx.OpLoad(ctx.U1, ctx.front_face),
                            ctx.OpBitcast(ctx.F32[1], ctx.Const(std::numeric_limits<u32>::max())),
                            ctx.f32_zero_value);
    case IR::Attribute::PointSpriteS:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.point_coord, ctx.u32_zero_value));
    case IR::Attribute::PointSpriteT:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.point_coord, ctx.Const(1U)));
    case IR::Attribute::TessellationEvaluationPointU:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.u32_zero_value));
    case IR::Attribute::TessellationEvaluationPointV:
        return ctx.OpLoad(ctx.F32[1],
                          ctx.OpAccessChain(ctx.input_f32, ctx.tess_coord, ctx.Const(1U)));
    default:
        throw NotImplementedException("Read attribute {}", attr);
    }
}

Id EmitGetAttributeU32(EmitContext& ctx, IR::Attribute attr, Id) {
    switch (attr) {
    case IR::Attribute::PrimitiveId:
        return ctx.OpLoad(ctx.U32[1], ctx.primitive_id);
    case IR::Attribute::InstanceId:
        if (ctx.profile.support_vertex_instance_id) {
            return ctx.OpLoad(ctx.U32[1], ctx.instance_id);
        } else {
            const Id index{ctx.OpLoad(ctx.U32[1], ctx.instance_index)};
            const Id base{ctx.OpLoad(ctx.U32[1], ctx.base_instance)};
            return ctx.OpISub(ctx.U32[1], index, base);
        }
    case IR::Attribute::VertexId:
        if (ctx.profile.support_vertex_instance_id) {
            return ctx.OpLoad(ctx.U32[1], ctx.vertex_id);
        } else {
            // const Id index{ctx.OpLoad(ctx.U32[1], ctx.vertex_index)};
            // const Id base{ctx.OpLoad(ctx.U32[1], ctx.base_vertex)};
            return ctx.OpLoad(ctx.U32[1], ctx.vertex_index);
        }
    case IR::Attribute::BaseInstance:
        return ctx.OpLoad(ctx.U32[1], ctx.base_instance);
    case IR::Attribute::BaseVertex:
        return ctx.OpLoad(ctx.U32[1], ctx.base_vertex);
    case IR::Attribute::DrawID:
        return ctx.OpLoad(ctx.U32[1], ctx.draw_index);
    default:
        throw NotImplementedException("Read U32 attribute {}", attr);
    }
}

void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, Id value, [[maybe_unused]] Id vertex) {
    const std::optional<OutAttr> output{OutputAttrPointer(ctx, attr)};
    if (!output) {
        return;
    }
    if (Sirit::ValidId(output->type)) {
        value = ctx.OpBitcast(output->type, value);
    }
    ctx.OpStore(output->pointer, value);
}

Id EmitGetAttributeIndexed(EmitContext& ctx, Id offset, Id vertex) {
    switch (ctx.stage) {
    case Stage::TessellationControl:
    case Stage::TessellationEval:
    case Stage::Geometry:
        return ctx.OpFunctionCall(ctx.F32[1], ctx.indexed_load_func, offset, vertex);
    default:
        return ctx.OpFunctionCall(ctx.F32[1], ctx.indexed_load_func, offset);
    }
}

void EmitSetAttributeIndexed(EmitContext& ctx, Id offset, Id value, [[maybe_unused]] Id vertex) {
    ctx.OpFunctionCall(ctx.void_id, ctx.indexed_store_func, offset, value);
}

Id EmitGetPatch(EmitContext& ctx, IR::Patch patch) {
    if (!IR::IsGeneric(patch)) {
        throw NotImplementedException("Non-generic patch load");
    }
    const u32 index{IR::GenericPatchIndex(patch)};
    const Id element{ctx.Const(IR::GenericPatchElement(patch))};
    const Id type{ctx.stage == Stage::TessellationControl ? ctx.output_f32 : ctx.input_f32};
    const Id pointer{ctx.OpAccessChain(type, ctx.patches.at(index), element)};
    return ctx.OpLoad(ctx.F32[1], pointer);
}

void EmitSetPatch(EmitContext& ctx, IR::Patch patch, Id value) {
    const Id pointer{[&] {
        if (IR::IsGeneric(patch)) {
            const u32 index{IR::GenericPatchIndex(patch)};
            const Id element{ctx.Const(IR::GenericPatchElement(patch))};
            return ctx.OpAccessChain(ctx.output_f32, ctx.patches.at(index), element);
        }
        switch (patch) {
        case IR::Patch::TessellationLodLeft:
        case IR::Patch::TessellationLodRight:
        case IR::Patch::TessellationLodTop:
        case IR::Patch::TessellationLodBottom: {
            const u32 index{static_cast<u32>(patch) - u32(IR::Patch::TessellationLodLeft)};
            const Id index_id{ctx.Const(index)};
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_outer, index_id);
        }
        case IR::Patch::TessellationLodInteriorU:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner,
                                     ctx.u32_zero_value);
        case IR::Patch::TessellationLodInteriorV:
            return ctx.OpAccessChain(ctx.output_f32, ctx.output_tess_level_inner, ctx.Const(1u));
        default:
            throw NotImplementedException("Patch {}", patch);
        }
    }()};
    ctx.OpStore(pointer, value);
}

void EmitSetFragColor(EmitContext& ctx, u32 index, u32 component, Id value) {
    const Id component_id{ctx.Const(component)};
    const Id pointer{ctx.OpAccessChain(ctx.output_f32, ctx.frag_color.at(index), component_id)};
    ctx.OpStore(pointer, value);
}

void EmitSetSampleMask(EmitContext& ctx, Id value) {
    const Id pointer{ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value)};
    ctx.OpStore(pointer, value);
}

void EmitSetFragDepth(EmitContext& ctx, Id value) {
    if (!ctx.runtime_info.convert_depth_mode || ctx.profile.support_native_ndc) {
        ctx.OpStore(ctx.frag_depth, value);
        return;
    }
    const Id unit{ctx.Const(0.5f)};
    const Id new_depth{ctx.OpFma(ctx.F32[1], value, unit, unit)};
    ctx.OpStore(ctx.frag_depth, new_depth);
}

void EmitGetZFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitGetSFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitGetCFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitGetOFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitSetZFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitSetSFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitSetCFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitSetOFlag(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitWorkgroupId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[3], ctx.workgroup_id);
}

Id EmitLocalInvocationId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id);
}

Id EmitInvocationId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[1], ctx.invocation_id);
}

Id EmitInvocationInfo(EmitContext& ctx) {
    switch (ctx.stage) {
    case Stage::TessellationControl:
    case Stage::TessellationEval:
        return ctx.OpShiftLeftLogical(ctx.U32[1], ctx.OpLoad(ctx.U32[1], ctx.patch_vertices_in),
                                      ctx.Const(16u));
    default:
        LOG_WARNING(Shader, "(STUBBED) called");
        return ctx.Const(0x00ff0000u);
    }
}

Id EmitSampleId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[1], ctx.sample_id);
}

Id EmitIsHelperInvocation(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U1, ctx.is_helper_invocation);
}

Id EmitYDirection(EmitContext& ctx) {
    return ctx.Const(ctx.runtime_info.y_negate ? -1.0f : 1.0f);
}

Id EmitResolutionDownFactor(EmitContext& ctx) {
    if (ctx.profile.unified_descriptor_binding) {
        const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[1])};
        const Id index{ctx.Const(ctx.rescaling_downfactor_member_index)};
        const Id pointer{ctx.OpAccessChain(pointer_type, ctx.rescaling_push_constants, index)};
        return ctx.OpLoad(ctx.F32[1], pointer);
    } else {
        const Id composite{ctx.OpLoad(ctx.F32[4], ctx.rescaling_uniform_constant)};
        return ctx.OpCompositeExtract(ctx.F32[1], composite, 2u);
    }
}

Id EmitRenderArea(EmitContext& ctx) {
    if (ctx.profile.unified_descriptor_binding) {
        const Id pointer_type{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[4])};
        const Id index{ctx.Const(ctx.render_are_member_index)};
        const Id pointer{ctx.OpAccessChain(pointer_type, ctx.render_area_push_constant, index)};
        return ctx.OpLoad(ctx.F32[4], pointer);
    } else {
        throw NotImplementedException("SPIR-V Instruction");
    }
}

Id EmitLoadLocal(EmitContext& ctx, Id word_offset) {
    const Id pointer{ctx.OpAccessChain(ctx.private_u32, ctx.local_memory, word_offset)};
    return ctx.OpLoad(ctx.U32[1], pointer);
}

void EmitWriteLocal(EmitContext& ctx, Id word_offset, Id value) {
    const Id pointer{ctx.OpAccessChain(ctx.private_u32, ctx.local_memory, word_offset)};
    ctx.OpStore(pointer, value);
}

} // namespace Shader::Backend::SPIRV
