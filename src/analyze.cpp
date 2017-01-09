/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "analyze.hpp"
#include "ast_render.hpp"
#include "config.h"
#include "error.hpp"
#include "ir.hpp"
#include "ir_print.hpp"
#include "os.hpp"
#include "parser.hpp"
#include "zig_llvm.hpp"

static const size_t default_backward_branch_quota = 1000;

static void resolve_enum_type(CodeGen *g, TypeTableEntry *enum_type);
static void resolve_struct_type(CodeGen *g, TypeTableEntry *struct_type);

static void resolve_struct_zero_bits(CodeGen *g, TypeTableEntry *struct_type);
static void resolve_enum_zero_bits(CodeGen *g, TypeTableEntry *enum_type);
static void resolve_union_zero_bits(CodeGen *g, TypeTableEntry *union_type);

AstNode *first_executing_node(AstNode *node) {
    switch (node->type) {
        case NodeTypeFnCallExpr:
            return first_executing_node(node->data.fn_call_expr.fn_ref_expr);
        case NodeTypeBinOpExpr:
            return first_executing_node(node->data.bin_op_expr.op1);
        case NodeTypeUnwrapErrorExpr:
            return first_executing_node(node->data.unwrap_err_expr.op1);
        case NodeTypeArrayAccessExpr:
            return first_executing_node(node->data.array_access_expr.array_ref_expr);
        case NodeTypeSliceExpr:
            return first_executing_node(node->data.slice_expr.array_ref_expr);
        case NodeTypeFieldAccessExpr:
            return first_executing_node(node->data.field_access_expr.struct_expr);
        case NodeTypeSwitchRange:
            return first_executing_node(node->data.switch_range.start);
        case NodeTypeRoot:
        case NodeTypeFnProto:
        case NodeTypeFnDef:
        case NodeTypeFnDecl:
        case NodeTypeParamDecl:
        case NodeTypeBlock:
        case NodeTypeReturnExpr:
        case NodeTypeDefer:
        case NodeTypeVariableDeclaration:
        case NodeTypeTypeDecl:
        case NodeTypeErrorValueDecl:
        case NodeTypeNumberLiteral:
        case NodeTypeStringLiteral:
        case NodeTypeCharLiteral:
        case NodeTypeSymbol:
        case NodeTypePrefixOpExpr:
        case NodeTypeUse:
        case NodeTypeBoolLiteral:
        case NodeTypeNullLiteral:
        case NodeTypeUndefinedLiteral:
        case NodeTypeZeroesLiteral:
        case NodeTypeThisLiteral:
        case NodeTypeIfBoolExpr:
        case NodeTypeIfVarExpr:
        case NodeTypeLabel:
        case NodeTypeGoto:
        case NodeTypeBreak:
        case NodeTypeContinue:
        case NodeTypeAsmExpr:
        case NodeTypeContainerDecl:
        case NodeTypeStructField:
        case NodeTypeStructValueField:
        case NodeTypeWhileExpr:
        case NodeTypeForExpr:
        case NodeTypeSwitchExpr:
        case NodeTypeSwitchProng:
        case NodeTypeArrayType:
        case NodeTypeErrorType:
        case NodeTypeTypeLiteral:
        case NodeTypeContainerInitExpr:
        case NodeTypeVarLiteral:
            return node;
    }
    zig_unreachable();
}

ErrorMsg *add_node_error(CodeGen *g, AstNode *node, Buf *msg) {
    // if this assert fails, then parseh generated code that
    // failed semantic analysis, which isn't supposed to happen
    assert(!node->owner->c_import_node);

    ErrorMsg *err = err_msg_create_with_line(node->owner->path, node->line, node->column,
            node->owner->source_code, node->owner->line_offsets, msg);

    g->errors.append(err);
    return err;
}

ErrorMsg *add_error_note(CodeGen *g, ErrorMsg *parent_msg, AstNode *node, Buf *msg) {
    // if this assert fails, then parseh generated code that
    // failed semantic analysis, which isn't supposed to happen
    assert(!node->owner->c_import_node);

    ErrorMsg *err = err_msg_create_with_line(node->owner->path, node->line, node->column,
            node->owner->source_code, node->owner->line_offsets, msg);

    err_msg_add_note(parent_msg, err);
    return err;
}

TypeTableEntry *new_type_table_entry(TypeTableEntryId id) {
    TypeTableEntry *entry = allocate<TypeTableEntry>(1);
    entry->arrays_by_size.init(2);
    entry->id = id;
    return entry;
}

static ScopeDecls **get_container_scope_ptr(TypeTableEntry *type_entry) {
    if (type_entry->id == TypeTableEntryIdStruct) {
        return &type_entry->data.structure.decls_scope;
    } else if (type_entry->id == TypeTableEntryIdEnum) {
        return &type_entry->data.enumeration.decls_scope;
    } else if (type_entry->id == TypeTableEntryIdUnion) {
        return &type_entry->data.unionation.decls_scope;
    }
    zig_unreachable();
}

ScopeDecls *get_container_scope(TypeTableEntry *type_entry) {
    return *get_container_scope_ptr(type_entry);
}

void init_scope(Scope *dest, ScopeId id, AstNode *source_node, Scope *parent) {
    dest->id = id;
    dest->source_node = source_node;
    dest->parent = parent;
}

ScopeDecls *create_decls_scope(AstNode *node, Scope *parent, TypeTableEntry *container_type, ImportTableEntry *import) {
    assert(node == nullptr || node->type == NodeTypeRoot || node->type == NodeTypeContainerDecl || node->type == NodeTypeFnCallExpr);
    ScopeDecls *scope = allocate<ScopeDecls>(1);
    init_scope(&scope->base, ScopeIdDecls, node, parent);
    scope->decl_table.init(4);
    scope->container_type = container_type;
    scope->import = import;
    return scope;
}

ScopeBlock *create_block_scope(AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeBlock);
    ScopeBlock *scope = allocate<ScopeBlock>(1);
    init_scope(&scope->base, ScopeIdBlock, node, parent);
    scope->label_table.init(1);
    return scope;
}

ScopeDefer *create_defer_scope(AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeDefer);
    ScopeDefer *scope = allocate<ScopeDefer>(1);
    init_scope(&scope->base, ScopeIdDefer, node, parent);
    return scope;
}

ScopeDeferExpr *create_defer_expr_scope(AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeDefer);
    ScopeDeferExpr *scope = allocate<ScopeDeferExpr>(1);
    init_scope(&scope->base, ScopeIdDeferExpr, node, parent);
    return scope;
}

Scope *create_var_scope(AstNode *node, Scope *parent, VariableTableEntry *var) {
    ScopeVarDecl *scope = allocate<ScopeVarDecl>(1);
    init_scope(&scope->base, ScopeIdVarDecl, node, parent);
    scope->var = var;
    return &scope->base;
}

ScopeCImport *create_cimport_scope(AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeFnCallExpr);
    ScopeCImport *scope = allocate<ScopeCImport>(1);
    init_scope(&scope->base, ScopeIdCImport, node, parent);
    buf_resize(&scope->buf, 0);
    return scope;
}

Scope *create_loop_scope(AstNode *node, Scope *parent) {
    assert(node->type == NodeTypeWhileExpr || node->type == NodeTypeForExpr);
    ScopeLoop *scope = allocate<ScopeLoop>(1);
    init_scope(&scope->base, ScopeIdLoop, node, parent);
    return &scope->base;
}

ScopeFnDef *create_fndef_scope(AstNode *node, Scope *parent, FnTableEntry *fn_entry) {
    assert(node->type == NodeTypeFnDef);
    ScopeFnDef *scope = allocate<ScopeFnDef>(1);
    init_scope(&scope->base, ScopeIdFnDef, node, parent);
    scope->fn_entry = fn_entry;
    return scope;
}

ImportTableEntry *get_scope_import(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;
            assert(decls_scope->import);
            return decls_scope->import;
        }
        scope = scope->parent;
    }
    zig_unreachable();
}

static TypeTableEntry *new_container_type_entry(TypeTableEntryId id, AstNode *source_node, Scope *parent_scope) {
    TypeTableEntry *entry = new_type_table_entry(id);
    *get_container_scope_ptr(entry) = create_decls_scope(source_node, parent_scope, entry, get_scope_import(parent_scope));
    return entry;
}


static size_t bits_needed_for_unsigned(uint64_t x) {
    if (x <= UINT8_MAX) {
        return 8;
    } else if (x <= UINT16_MAX) {
        return 16;
    } else if (x <= UINT32_MAX) {
        return 32;
    } else {
        return 64;
    }
}

bool type_is_complete(TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdVar:
            zig_unreachable();
        case TypeTableEntryIdStruct:
            return type_entry->data.structure.complete;
        case TypeTableEntryIdEnum:
            return type_entry->data.enumeration.complete;
        case TypeTableEntryIdUnion:
            return type_entry->data.unionation.complete;
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdEnumTag:
            return true;
    }
    zig_unreachable();
}

bool type_has_zero_bits_known(TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdVar:
            zig_unreachable();
        case TypeTableEntryIdStruct:
            return type_entry->data.structure.zero_bits_known;
        case TypeTableEntryIdEnum:
            return type_entry->data.enumeration.zero_bits_known;
        case TypeTableEntryIdUnion:
            return type_entry->data.unionation.zero_bits_known;
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdEnumTag:
            return true;
    }
    zig_unreachable();
}


uint64_t type_size(CodeGen *g, TypeTableEntry *type_entry) {
    if (type_has_bits(type_entry)) {
        return LLVMStoreSizeOfType(g->target_data_ref, type_entry->type_ref);
    } else {
        return 0;
    }
}

static bool is_slice(TypeTableEntry *type) {
    return type->id == TypeTableEntryIdStruct && type->data.structure.is_slice;
}

TypeTableEntry *get_smallest_unsigned_int_type(CodeGen *g, uint64_t x) {
    return get_int_type(g, false, bits_needed_for_unsigned(x));
}

TypeTableEntry *get_pointer_to_type(CodeGen *g, TypeTableEntry *child_type, bool is_const) {
    assert(child_type->id != TypeTableEntryIdInvalid);
    TypeTableEntry **parent_pointer = &child_type->pointer_parent[(is_const ? 1 : 0)];
    if (*parent_pointer) {
        return *parent_pointer;
    } else {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdPointer);

        const char *const_str = is_const ? "const " : "";
        buf_resize(&entry->name, 0);
        buf_appendf(&entry->name, "&%s%s", const_str, buf_ptr(&child_type->name));

        TypeTableEntry *canon_child_type = get_underlying_type(child_type);
        assert(canon_child_type->id != TypeTableEntryIdInvalid);

        entry->zero_bits = !type_has_bits(canon_child_type);

        if (!entry->zero_bits) {
            entry->type_ref = LLVMPointerType(child_type->type_ref, 0);

            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);
            assert(child_type->di_type);
            entry->di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, child_type->di_type,
                    debug_size_in_bits, debug_align_in_bits, buf_ptr(&entry->name));
        } else {
            entry->di_type = g->builtin_types.entry_void->di_type;
        }

        entry->data.pointer.child_type = child_type;
        entry->data.pointer.is_const = is_const;

        *parent_pointer = entry;
        return entry;
    }
}

TypeTableEntry *get_maybe_type(CodeGen *g, TypeTableEntry *child_type) {
    if (child_type->maybe_parent) {
        TypeTableEntry *entry = child_type->maybe_parent;
        return entry;
    } else {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdMaybe);
        assert(child_type->type_ref);
        assert(child_type->di_type);

        buf_resize(&entry->name, 0);
        buf_appendf(&entry->name, "?%s", buf_ptr(&child_type->name));

        if (child_type->id == TypeTableEntryIdPointer ||
            child_type->id == TypeTableEntryIdFn)
        {
            // this is an optimization but also is necessary for calling C
            // functions where all pointers are maybe pointers
            // function types are technically pointers
            entry->type_ref = child_type->type_ref;
            entry->di_type = child_type->di_type;
        } else {
            // create a struct with a boolean whether this is the null value
            LLVMTypeRef elem_types[] = {
                child_type->type_ref,
                LLVMInt1Type(),
            };
            entry->type_ref = LLVMStructType(elem_types, 2, false);


            ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
            ZigLLVMDIFile *di_file = nullptr;
            unsigned line = 0;
            entry->di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
                ZigLLVMTag_DW_structure_type(), buf_ptr(&entry->name),
                compile_unit_scope, di_file, line);

            uint64_t val_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, child_type->type_ref);
            uint64_t val_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, child_type->type_ref);
            uint64_t val_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, 0);

            TypeTableEntry *bool_type = g->builtin_types.entry_bool;
            uint64_t maybe_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, bool_type->type_ref);
            uint64_t maybe_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, bool_type->type_ref);
            uint64_t maybe_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, 1);

            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);

            ZigLLVMDIType *di_element_types[] = {
                ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                        "val", di_file, line,
                        val_debug_size_in_bits,
                        val_debug_align_in_bits,
                        val_offset_in_bits,
                        0, child_type->di_type),
                ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                        "maybe", di_file, line,
                        maybe_debug_size_in_bits,
                        maybe_debug_align_in_bits,
                        maybe_offset_in_bits,
                        0, child_type->di_type),
            };
            ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                    compile_unit_scope,
                    buf_ptr(&entry->name),
                    di_file, line, debug_size_in_bits, debug_align_in_bits, 0,
                    nullptr, di_element_types, 2, 0, nullptr, "");

            ZigLLVMReplaceTemporary(g->dbuilder, entry->di_type, replacement_di_type);
            entry->di_type = replacement_di_type;
        }

        entry->data.maybe.child_type = child_type;

        child_type->maybe_parent = entry;
        return entry;
    }
}

TypeTableEntry *get_error_type(CodeGen *g, TypeTableEntry *child_type) {
    if (child_type->error_parent)
        return child_type->error_parent;

    TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdErrorUnion);
    assert(child_type->type_ref);
    assert(child_type->di_type);
    ensure_complete_type(g, child_type);

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "%%%s", buf_ptr(&child_type->name));

    entry->data.error.child_type = child_type;

    if (!type_has_bits(child_type)) {
        entry->type_ref = g->err_tag_type->type_ref;
        entry->di_type = g->err_tag_type->di_type;

    } else {
        LLVMTypeRef elem_types[] = {
            g->err_tag_type->type_ref,
            child_type->type_ref,
        };
        entry->type_ref = LLVMStructType(elem_types, 2, false);

        ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
        ZigLLVMDIFile *di_file = nullptr;
        unsigned line = 0;
        entry->di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
            ZigLLVMTag_DW_structure_type(), buf_ptr(&entry->name),
            compile_unit_scope, di_file, line);

        uint64_t tag_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, g->err_tag_type->type_ref);
        uint64_t tag_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, g->err_tag_type->type_ref);
        uint64_t tag_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, err_union_err_index);

        uint64_t value_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, child_type->type_ref);
        uint64_t value_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, child_type->type_ref);
        uint64_t value_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref,
                err_union_payload_index);

        uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
        uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);

        ZigLLVMDIType *di_element_types[] = {
            ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                    "tag", di_file, line,
                    tag_debug_size_in_bits,
                    tag_debug_align_in_bits,
                    tag_offset_in_bits,
                    0, child_type->di_type),
            ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                    "value", di_file, line,
                    value_debug_size_in_bits,
                    value_debug_align_in_bits,
                    value_offset_in_bits,
                    0, child_type->di_type),
        };

        ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                compile_unit_scope,
                buf_ptr(&entry->name),
                di_file, line,
                debug_size_in_bits,
                debug_align_in_bits,
                0,
                nullptr, di_element_types, 2, 0, nullptr, "");

        ZigLLVMReplaceTemporary(g->dbuilder, entry->di_type, replacement_di_type);
        entry->di_type = replacement_di_type;
    }

    child_type->error_parent = entry;
    return entry;
}

TypeTableEntry *get_array_type(CodeGen *g, TypeTableEntry *child_type, uint64_t array_size) {
    auto existing_entry = child_type->arrays_by_size.maybe_get(array_size);
    if (existing_entry) {
        TypeTableEntry *entry = existing_entry->value;
        return entry;
    } else {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdArray);
        entry->zero_bits = (array_size == 0) || child_type->zero_bits;

        buf_resize(&entry->name, 0);
        buf_appendf(&entry->name, "[%" PRIu64 "]%s", array_size, buf_ptr(&child_type->name));

        if (!entry->zero_bits) {
            entry->type_ref = child_type->type_ref ? LLVMArrayType(child_type->type_ref, array_size) : nullptr;

            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);

            entry->di_type = ZigLLVMCreateDebugArrayType(g->dbuilder, debug_size_in_bits,
                    debug_align_in_bits, child_type->di_type, array_size);
        }
        entry->data.array.child_type = child_type;
        entry->data.array.len = array_size;

        child_type->arrays_by_size.put(array_size, entry);
        return entry;
    }
}

static void slice_type_common_init(CodeGen *g, TypeTableEntry *child_type,
        bool is_const, TypeTableEntry *entry)
{
    TypeTableEntry *pointer_type = get_pointer_to_type(g, child_type, is_const);

    unsigned element_count = 2;
    entry->data.structure.is_packed = false;
    entry->data.structure.is_slice = true;
    entry->data.structure.src_field_count = element_count;
    entry->data.structure.gen_field_count = element_count;
    entry->data.structure.fields = allocate<TypeStructField>(element_count);
    entry->data.structure.fields[slice_ptr_index].name = buf_create_from_str("ptr");
    entry->data.structure.fields[slice_ptr_index].type_entry = pointer_type;
    entry->data.structure.fields[slice_ptr_index].src_index = slice_ptr_index;
    entry->data.structure.fields[slice_ptr_index].gen_index = 0;
    entry->data.structure.fields[slice_len_index].name = buf_create_from_str("len");
    entry->data.structure.fields[slice_len_index].type_entry = g->builtin_types.entry_usize;
    entry->data.structure.fields[slice_len_index].src_index = slice_len_index;
    entry->data.structure.fields[slice_len_index].gen_index = 1;

    assert(type_has_zero_bits_known(child_type));
    if (child_type->zero_bits) {
        entry->data.structure.gen_field_count = 1;
        entry->data.structure.fields[slice_ptr_index].gen_index = SIZE_MAX;
        entry->data.structure.fields[slice_len_index].gen_index = 0;
    }
}

TypeTableEntry *get_slice_type(CodeGen *g, TypeTableEntry *child_type, bool is_const) {
    assert(child_type->id != TypeTableEntryIdInvalid);
    TypeTableEntry **parent_pointer = &child_type->unknown_size_array_parent[(is_const ? 1 : 0)];

    if (*parent_pointer) {
        return *parent_pointer;
    } else if (is_const) {
        TypeTableEntry *var_peer = get_slice_type(g, child_type, false);
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdStruct);

        buf_resize(&entry->name, 0);
        buf_appendf(&entry->name, "[]const %s", buf_ptr(&child_type->name));

        slice_type_common_init(g, child_type, is_const, entry);

        entry->type_ref = var_peer->type_ref;
        entry->di_type = var_peer->di_type;
        entry->data.structure.complete = true;
        entry->data.structure.zero_bits_known = true;

        *parent_pointer = entry;
        return entry;
    } else {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdStruct);

        // If the child type is []const T then we need to make sure the type ref
        // and debug info is the same as if the child type were []T.
        if (is_slice(child_type)) {
            TypeTableEntry *ptr_type = child_type->data.structure.fields[slice_ptr_index].type_entry;
            assert(ptr_type->id == TypeTableEntryIdPointer);
            if (ptr_type->data.pointer.is_const) {
                TypeTableEntry *non_const_child_type = get_slice_type(g,
                    ptr_type->data.pointer.child_type, false);
                TypeTableEntry *var_peer = get_slice_type(g, non_const_child_type, false);

                entry->type_ref = var_peer->type_ref;
                entry->di_type = var_peer->di_type;
            }
        }

        buf_resize(&entry->name, 0);
        buf_appendf(&entry->name, "[]%s", buf_ptr(&child_type->name));

        slice_type_common_init(g, child_type, is_const, entry);

        if (!entry->type_ref) {
            entry->type_ref = LLVMStructCreateNamed(LLVMGetGlobalContext(), buf_ptr(&entry->name));

            ZigLLVMDIScope *compile_unit_scope = ZigLLVMCompileUnitToScope(g->compile_unit);
            ZigLLVMDIFile *di_file = nullptr;
            unsigned line = 0;
            entry->di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
                ZigLLVMTag_DW_structure_type(), buf_ptr(&entry->name),
                compile_unit_scope, di_file, line);

            if (child_type->zero_bits) {
                LLVMTypeRef element_types[] = {
                    g->builtin_types.entry_usize->type_ref,
                };
                LLVMStructSetBody(entry->type_ref, element_types, 1, false);

                TypeTableEntry *usize_type = g->builtin_types.entry_usize;
                uint64_t len_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, usize_type->type_ref);
                uint64_t len_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, usize_type->type_ref);
                uint64_t len_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, 0);

                uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
                uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);

                ZigLLVMDIType *di_element_types[] = {
                    ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                            "len", di_file, line,
                            len_debug_size_in_bits,
                            len_debug_align_in_bits,
                            len_offset_in_bits,
                            0, usize_type->di_type),
                };
                ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                        compile_unit_scope,
                        buf_ptr(&entry->name),
                        di_file, line, debug_size_in_bits, debug_align_in_bits, 0,
                        nullptr, di_element_types, 1, 0, nullptr, "");

                ZigLLVMReplaceTemporary(g->dbuilder, entry->di_type, replacement_di_type);
                entry->di_type = replacement_di_type;
            } else {
                TypeTableEntry *pointer_type = get_pointer_to_type(g, child_type, is_const);

                unsigned element_count = 2;
                LLVMTypeRef element_types[] = {
                    pointer_type->type_ref,
                    g->builtin_types.entry_usize->type_ref,
                };
                LLVMStructSetBody(entry->type_ref, element_types, element_count, false);


                uint64_t ptr_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, pointer_type->type_ref);
                uint64_t ptr_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, pointer_type->type_ref);
                uint64_t ptr_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, 0);

                TypeTableEntry *usize_type = g->builtin_types.entry_usize;
                uint64_t len_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, usize_type->type_ref);
                uint64_t len_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, usize_type->type_ref);
                uint64_t len_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, entry->type_ref, 1);

                uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, entry->type_ref);
                uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, entry->type_ref);

                ZigLLVMDIType *di_element_types[] = {
                    ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                            "ptr", di_file, line,
                            ptr_debug_size_in_bits,
                            ptr_debug_align_in_bits,
                            ptr_offset_in_bits,
                            0, pointer_type->di_type),
                    ZigLLVMCreateDebugMemberType(g->dbuilder, ZigLLVMTypeToScope(entry->di_type),
                            "len", di_file, line,
                            len_debug_size_in_bits,
                            len_debug_align_in_bits,
                            len_offset_in_bits,
                            0, usize_type->di_type),
                };
                ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                        compile_unit_scope,
                        buf_ptr(&entry->name),
                        di_file, line, debug_size_in_bits, debug_align_in_bits, 0,
                        nullptr, di_element_types, 2, 0, nullptr, "");

                ZigLLVMReplaceTemporary(g->dbuilder, entry->di_type, replacement_di_type);
                entry->di_type = replacement_di_type;
            }
        }


        entry->data.structure.complete = true;
        entry->data.structure.zero_bits_known = true;

        *parent_pointer = entry;
        return entry;
    }
}

TypeTableEntry *get_typedecl_type(CodeGen *g, const char *name, TypeTableEntry *child_type) {
    TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdTypeDecl);

    buf_init_from_str(&entry->name, name);

    entry->type_ref = child_type->type_ref;
    entry->di_type = child_type->di_type;
    entry->zero_bits = child_type->zero_bits;
    entry->data.type_decl.child_type = child_type;
    entry->data.type_decl.canonical_type = get_underlying_type(child_type);

    return entry;
}

TypeTableEntry *get_bound_fn_type(CodeGen *g, FnTableEntry *fn_entry) {
    TypeTableEntry *fn_type = fn_entry->type_entry;
    assert(fn_type->id == TypeTableEntryIdFn);
    if (fn_type->data.fn.bound_fn_parent)
        return fn_type->data.fn.bound_fn_parent;

    TypeTableEntry *bound_fn_type = new_type_table_entry(TypeTableEntryIdBoundFn);
    bound_fn_type->data.bound_fn.fn_type = fn_type;
    bound_fn_type->zero_bits = true;

    buf_resize(&bound_fn_type->name, 0);
    buf_appendf(&bound_fn_type->name, "(bound %s)", buf_ptr(&fn_type->name));

    fn_type->data.fn.bound_fn_parent = bound_fn_type;
    return bound_fn_type;
}

TypeTableEntry *get_fn_type(CodeGen *g, FnTypeId *fn_type_id) {
    auto table_entry = g->fn_type_table.maybe_get(fn_type_id);
    if (table_entry) {
        return table_entry->value;
    }
    ensure_complete_type(g, fn_type_id->return_type);

    TypeTableEntry *fn_type = new_type_table_entry(TypeTableEntryIdFn);
    fn_type->data.fn.fn_type_id = *fn_type_id;

    if (fn_type_id->is_cold) {
        fn_type->data.fn.calling_convention = LLVMColdCallConv;
    } else if (fn_type_id->is_extern) {
        fn_type->data.fn.calling_convention = LLVMCCallConv;
    } else {
        fn_type->data.fn.calling_convention = LLVMFastCallConv;
    }

    bool skip_debug_info = false;

    // populate the name of the type
    buf_resize(&fn_type->name, 0);
    const char *extern_str = fn_type_id->is_extern ? "extern " : "";
    const char *naked_str = fn_type_id->is_naked ? "naked " : "";
    const char *cold_str = fn_type_id->is_cold ? "cold " : "";
    buf_appendf(&fn_type->name, "%s%s%sfn(", extern_str, naked_str, cold_str);
    for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
        FnTypeParamInfo *param_info = &fn_type_id->param_info[i];

        TypeTableEntry *param_type = param_info->type;
        const char *comma = (i == 0) ? "" : ", ";
        const char *noalias_str = param_info->is_noalias ? "noalias " : "";
        buf_appendf(&fn_type->name, "%s%s%s", comma, noalias_str, buf_ptr(&param_type->name));

        skip_debug_info = skip_debug_info || !param_type->di_type;
    }

    if (fn_type_id->is_var_args) {
        const char *comma = (fn_type_id->param_count == 0) ? "" : ", ";
        buf_appendf(&fn_type->name, "%s...", comma);
    }
    buf_appendf(&fn_type->name, ")");
    if (fn_type_id->return_type->id != TypeTableEntryIdVoid) {
        buf_appendf(&fn_type->name, " -> %s", buf_ptr(&fn_type_id->return_type->name));
    }
    skip_debug_info = skip_debug_info || !fn_type_id->return_type->di_type;

    // next, loop over the parameters again and compute debug information
    // and codegen information
    if (!skip_debug_info) {
        bool first_arg_return = !fn_type_id->is_extern && handle_is_ptr(fn_type_id->return_type);
        // +1 for maybe making the first argument the return value
        LLVMTypeRef *gen_param_types = allocate<LLVMTypeRef>(1 + fn_type_id->param_count);
        // +1 because 0 is the return type and +1 for maybe making first arg ret val
        ZigLLVMDIType **param_di_types = allocate<ZigLLVMDIType*>(2 + fn_type_id->param_count);
        param_di_types[0] = fn_type_id->return_type->di_type;
        size_t gen_param_index = 0;
        TypeTableEntry *gen_return_type;
        if (!type_has_bits(fn_type_id->return_type)) {
            gen_return_type = g->builtin_types.entry_void;
        } else if (first_arg_return) {
            TypeTableEntry *gen_type = get_pointer_to_type(g, fn_type_id->return_type, false);
            gen_param_types[gen_param_index] = gen_type->type_ref;
            gen_param_index += 1;
            // after the gen_param_index += 1 because 0 is the return type
            param_di_types[gen_param_index] = gen_type->di_type;
            gen_return_type = g->builtin_types.entry_void;
        } else {
            gen_return_type = fn_type_id->return_type;
        }
        fn_type->data.fn.gen_return_type = gen_return_type;

        fn_type->data.fn.gen_param_info = allocate<FnGenParamInfo>(fn_type_id->param_count);
        for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
            FnTypeParamInfo *src_param_info = &fn_type->data.fn.fn_type_id.param_info[i];
            TypeTableEntry *type_entry = src_param_info->type;
            FnGenParamInfo *gen_param_info = &fn_type->data.fn.gen_param_info[i];

            gen_param_info->src_index = i;
            gen_param_info->gen_index = SIZE_MAX;

            ensure_complete_type(g, type_entry);
            if (type_has_bits(type_entry)) {
                TypeTableEntry *gen_type;
                if (handle_is_ptr(type_entry)) {
                    gen_type = get_pointer_to_type(g, type_entry, true);
                    gen_param_info->is_byval = true;
                } else {
                    gen_type = type_entry;
                }
                gen_param_types[gen_param_index] = gen_type->type_ref;
                gen_param_info->gen_index = gen_param_index;
                gen_param_info->type = gen_type;

                gen_param_index += 1;

                // after the gen_param_index += 1 because 0 is the return type
                param_di_types[gen_param_index] = gen_type->di_type;
            }
        }

        fn_type->data.fn.gen_param_count = gen_param_index;

        fn_type->data.fn.raw_type_ref = LLVMFunctionType(gen_return_type->type_ref,
                gen_param_types, gen_param_index, fn_type_id->is_var_args);
        fn_type->type_ref = LLVMPointerType(fn_type->data.fn.raw_type_ref, 0);
        fn_type->di_type = ZigLLVMCreateSubroutineType(g->dbuilder, param_di_types, gen_param_index + 1, 0);
    }

    g->fn_type_table.put(&fn_type->data.fn.fn_type_id, fn_type);

    return fn_type;
}

static TypeTableEntryId container_to_type(ContainerKind kind) {
    switch (kind) {
        case ContainerKindStruct:
            return TypeTableEntryIdStruct;
        case ContainerKindEnum:
            return TypeTableEntryIdEnum;
        case ContainerKindUnion:
            return TypeTableEntryIdUnion;
    }
    zig_unreachable();
}

TypeTableEntry *get_partial_container_type(CodeGen *g, Scope *scope, ContainerKind kind, AstNode *decl_node, const char *name) {
    TypeTableEntryId type_id = container_to_type(kind);
    TypeTableEntry *entry = new_container_type_entry(type_id, decl_node, scope);

    switch (kind) {
        case ContainerKindStruct:
            entry->data.structure.decl_node = decl_node;
            entry->data.structure.is_extern = decl_node->data.container_decl.is_extern;
            break;
        case ContainerKindEnum:
            entry->data.enumeration.decl_node = decl_node;
            entry->data.enumeration.is_extern = decl_node->data.container_decl.is_extern;
            break;
        case ContainerKindUnion:
            entry->data.unionation.decl_node = decl_node;
            entry->data.unionation.is_extern = decl_node->data.container_decl.is_extern;
            break;
    }

    unsigned line = decl_node->line;

    ImportTableEntry *import = get_scope_import(scope);
    entry->type_ref = LLVMStructCreateNamed(LLVMGetGlobalContext(), name);
    entry->di_type = ZigLLVMCreateReplaceableCompositeType(g->dbuilder,
        ZigLLVMTag_DW_structure_type(), name,
        ZigLLVMFileToScope(import->di_file), import->di_file, line + 1);

    buf_init_from_str(&entry->name, name);

    return entry;
}

TypeTableEntry *get_underlying_type(TypeTableEntry *type_entry) {
    if (type_entry->id == TypeTableEntryIdTypeDecl) {
        return type_entry->data.type_decl.canonical_type;
    } else {
        return type_entry;
    }
}

static IrInstruction *analyze_const_value(CodeGen *g, Scope *scope, AstNode *node, TypeTableEntry *type_entry, Buf *type_name) {
    size_t backward_branch_count = 0;
    return ir_eval_const_value(g, scope, node, type_entry,
            &backward_branch_count, default_backward_branch_quota,
            nullptr, nullptr, node, type_name, nullptr);
}

TypeTableEntry *analyze_type_expr(CodeGen *g, Scope *scope, AstNode *node) {
    IrInstruction *result = analyze_const_value(g, scope, node, g->builtin_types.entry_type, nullptr);
    if (result->value.type->id == TypeTableEntryIdInvalid)
        return g->builtin_types.entry_invalid;

    assert(result->value.special != ConstValSpecialRuntime);
    return result->value.data.x_type;
}

static TypeTableEntry *get_generic_fn_type(CodeGen *g, FnTypeId *fn_type_id) {
    TypeTableEntry *fn_type = new_type_table_entry(TypeTableEntryIdFn);
    buf_init_from_str(&fn_type->name, "fn(");
    size_t i = 0;
    for (; i < fn_type_id->next_param_index; i += 1) {
        const char *comma_str = (i == 0) ? "" : ",";
        buf_appendf(&fn_type->name, "%s%s", comma_str,
            buf_ptr(&fn_type_id->param_info[i].type->name));
    }
    for (; i < fn_type_id->param_count; i += 1) {
        const char *comma_str = (i == 0) ? "" : ",";
        buf_appendf(&fn_type->name, "%svar", comma_str);
    }
    buf_appendf(&fn_type->name, ")->var");

    fn_type->data.fn.fn_type_id = *fn_type_id;
    fn_type->data.fn.is_generic = true;
    fn_type->zero_bits = true;
    return fn_type;
}

void init_fn_type_id(FnTypeId *fn_type_id, AstNode *proto_node) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    fn_type_id->is_extern = fn_proto->is_extern || (fn_proto->visib_mod == VisibModExport);
    fn_type_id->is_naked = fn_proto->is_nakedcc;
    fn_type_id->is_cold = fn_proto->is_coldcc;
    fn_type_id->param_count = fn_proto->params.length;
    fn_type_id->param_info = allocate_nonzero<FnTypeParamInfo>(fn_type_id->param_count);
    fn_type_id->next_param_index = 0;
    fn_type_id->is_var_args = fn_proto->is_var_args;
}

static TypeTableEntry *analyze_fn_type(CodeGen *g, AstNode *proto_node, Scope *child_scope) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    FnTypeId fn_type_id = {0};
    init_fn_type_id(&fn_type_id, proto_node);

    for (; fn_type_id.next_param_index < fn_type_id.param_count; fn_type_id.next_param_index += 1) {
        AstNode *param_node = fn_proto->params.at(fn_type_id.next_param_index);
        assert(param_node->type == NodeTypeParamDecl);

        bool param_is_inline = param_node->data.param_decl.is_inline;

        if (param_is_inline) {
            if (fn_type_id.is_extern) {
                add_node_error(g, param_node,
                        buf_sprintf("inline parameter not allowed in extern function"));
                return g->builtin_types.entry_invalid;
            }
            return get_generic_fn_type(g, &fn_type_id);
        }

        TypeTableEntry *type_entry = analyze_type_expr(g, child_scope, param_node->data.param_decl.type);

        switch (type_entry->id) {
            case TypeTableEntryIdInvalid:
                return g->builtin_types.entry_invalid;
            case TypeTableEntryIdUnreachable:
            case TypeTableEntryIdUndefLit:
            case TypeTableEntryIdNullLit:
                add_node_error(g, param_node->data.param_decl.type,
                    buf_sprintf("parameter of type '%s' not allowed", buf_ptr(&type_entry->name)));
                return g->builtin_types.entry_invalid;
            case TypeTableEntryIdVar:
                if (fn_type_id.is_extern) {
                    add_node_error(g, param_node->data.param_decl.type,
                            buf_sprintf("parameter of type 'var' not allowed in extern function"));
                    return g->builtin_types.entry_invalid;
                }
                return get_generic_fn_type(g, &fn_type_id);
            case TypeTableEntryIdNumLitFloat:
            case TypeTableEntryIdNumLitInt:
            case TypeTableEntryIdNamespace:
            case TypeTableEntryIdBlock:
            case TypeTableEntryIdBoundFn:
            case TypeTableEntryIdMetaType:
                add_node_error(g, param_node->data.param_decl.type,
                    buf_sprintf("parameter of type '%s' must be declared inline",
                    buf_ptr(&type_entry->name)));
                return g->builtin_types.entry_invalid;
            case TypeTableEntryIdVoid:
            case TypeTableEntryIdBool:
            case TypeTableEntryIdInt:
            case TypeTableEntryIdFloat:
            case TypeTableEntryIdPointer:
            case TypeTableEntryIdArray:
            case TypeTableEntryIdStruct:
            case TypeTableEntryIdMaybe:
            case TypeTableEntryIdErrorUnion:
            case TypeTableEntryIdPureError:
            case TypeTableEntryIdEnum:
            case TypeTableEntryIdUnion:
            case TypeTableEntryIdFn:
            case TypeTableEntryIdTypeDecl:
            case TypeTableEntryIdEnumTag:
                break;
        }
        FnTypeParamInfo *param_info = &fn_type_id.param_info[fn_type_id.next_param_index];
        param_info->type = type_entry;
        param_info->is_noalias = param_node->data.param_decl.is_noalias;
    }

    fn_type_id.return_type = analyze_type_expr(g, child_scope, fn_proto->return_type);

    switch (fn_type_id.return_type->id) {
        case TypeTableEntryIdInvalid:
            return g->builtin_types.entry_invalid;

        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
            add_node_error(g, fn_proto->return_type,
                buf_sprintf("return type '%s' not allowed", buf_ptr(&fn_type_id.return_type->name)));
            return g->builtin_types.entry_invalid;

        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdVar:
        case TypeTableEntryIdMetaType:
            if (fn_type_id.is_extern) {
                add_node_error(g, fn_proto->return_type,
                    buf_sprintf("return type '%s' not allowed in extern function",
                    buf_ptr(&fn_type_id.return_type->name)));
                return g->builtin_types.entry_invalid;
            }
            return get_generic_fn_type(g, &fn_type_id);
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdEnumTag:
            break;
    }

    return get_fn_type(g, &fn_type_id);
}

static TypeTableEntry *create_enum_tag_type(CodeGen *g, TypeTableEntry *enum_type, TypeTableEntry *int_type) {
    TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdEnumTag);

    buf_resize(&entry->name, 0);
    buf_appendf(&entry->name, "@enumTagType(%s)", buf_ptr(&enum_type->name));

    entry->data.enum_tag.enum_type = enum_type;
    entry->data.enum_tag.int_type = int_type;
    entry->type_ref = int_type->type_ref;
    entry->di_type = int_type->di_type;
    entry->zero_bits = int_type->zero_bits;

    return entry;
}

static void resolve_enum_type(CodeGen *g, TypeTableEntry *enum_type) {
    // if you change this logic you likely must also change similar logic in parseh.cpp
    assert(enum_type->id == TypeTableEntryIdEnum);

    resolve_enum_zero_bits(g, enum_type);
    if (enum_type->data.enumeration.is_invalid)
        return;

    AstNode *decl_node = enum_type->data.enumeration.decl_node;

    if (enum_type->data.enumeration.embedded_in_current) {
        if (!enum_type->data.enumeration.reported_infinite_err) {
            enum_type->data.enumeration.reported_infinite_err = true;
            add_node_error(g, decl_node, buf_sprintf("enum '%s' contains itself", buf_ptr(&enum_type->name)));
        }
        return;
    }

    assert(!enum_type->data.enumeration.zero_bits_loop_flag);
    assert(decl_node->type == NodeTypeContainerDecl);
    assert(enum_type->di_type);

    uint32_t field_count = enum_type->data.enumeration.src_field_count;

    assert(enum_type->data.enumeration.fields);
    ZigLLVMDIEnumerator **di_enumerators = allocate<ZigLLVMDIEnumerator*>(field_count);

    uint32_t gen_field_count = enum_type->data.enumeration.gen_field_count;
    ZigLLVMDIType **union_inner_di_types = allocate<ZigLLVMDIType*>(gen_field_count);

    TypeTableEntry *biggest_union_member = nullptr;
    uint64_t biggest_align_in_bits = 0;
    uint64_t biggest_union_member_size_in_bits = 0;

    Scope *scope = &enum_type->data.enumeration.decls_scope->base;
    ImportTableEntry *import = get_scope_import(scope);

    // set temporary flag
    enum_type->data.enumeration.embedded_in_current = true;

    for (uint32_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        TypeEnumField *type_enum_field = &enum_type->data.enumeration.fields[i];
        TypeTableEntry *field_type = type_enum_field->type_entry;

        di_enumerators[i] = ZigLLVMCreateDebugEnumerator(g->dbuilder, buf_ptr(type_enum_field->name), i);

        ensure_complete_type(g, field_type);
        if (field_type->id == TypeTableEntryIdInvalid) {
            enum_type->data.enumeration.is_invalid = true;
            continue;
        }

        enum_type->size_depends_on_compile_var = enum_type->size_depends_on_compile_var ||
            field_type->size_depends_on_compile_var;

        if (!type_has_bits(field_type))
            continue;

        uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, field_type->type_ref);
        uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, field_type->type_ref);

        assert(debug_size_in_bits > 0);
        assert(debug_align_in_bits > 0);

        union_inner_di_types[type_enum_field->gen_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(enum_type->di_type), buf_ptr(type_enum_field->name),
                import->di_file, field_node->line + 1,
                debug_size_in_bits,
                debug_align_in_bits,
                0,
                0, field_type->di_type);

        biggest_align_in_bits = max(biggest_align_in_bits, debug_align_in_bits);

        if (!biggest_union_member ||
            debug_size_in_bits > biggest_union_member_size_in_bits)
        {
            biggest_union_member = field_type;
            biggest_union_member_size_in_bits = debug_size_in_bits;
        }
    }

    // unset temporary flag
    enum_type->data.enumeration.embedded_in_current = false;
    enum_type->data.enumeration.complete = true;

    if (!enum_type->data.enumeration.is_invalid) {
        enum_type->data.enumeration.union_type = biggest_union_member;

        TypeTableEntry *tag_int_type = get_smallest_unsigned_int_type(g, field_count);
        TypeTableEntry *tag_type_entry = create_enum_tag_type(g, enum_type, tag_int_type);
        enum_type->data.enumeration.tag_type = tag_type_entry;

        if (biggest_union_member) {
            // create llvm type for union
            LLVMTypeRef union_element_type = biggest_union_member->type_ref;
            LLVMTypeRef union_type_ref = LLVMStructType(&union_element_type, 1, false);

            // create llvm type for root struct
            LLVMTypeRef root_struct_element_types[] = {
                tag_type_entry->type_ref,
                union_type_ref,
            };
            LLVMStructSetBody(enum_type->type_ref, root_struct_element_types, 2, false);

            // create debug type for tag
            uint64_t tag_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, tag_type_entry->type_ref);
            uint64_t tag_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, tag_type_entry->type_ref);
            ZigLLVMDIType *tag_di_type = ZigLLVMCreateDebugEnumerationType(g->dbuilder,
                    ZigLLVMTypeToScope(enum_type->di_type), "AnonEnum", import->di_file, decl_node->line + 1,
                    tag_debug_size_in_bits, tag_debug_align_in_bits, di_enumerators, field_count,
                    tag_type_entry->di_type, "");

            // create debug type for union
            ZigLLVMDIType *union_di_type = ZigLLVMCreateDebugUnionType(g->dbuilder,
                    ZigLLVMTypeToScope(enum_type->di_type), "AnonUnion", import->di_file, decl_node->line + 1,
                    biggest_union_member_size_in_bits, biggest_align_in_bits, 0, union_inner_di_types,
                    gen_field_count, 0, "");

            // create debug types for members of root struct
            uint64_t tag_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, enum_type->type_ref, 0);
            ZigLLVMDIType *tag_member_di_type = ZigLLVMCreateDebugMemberType(g->dbuilder,
                    ZigLLVMTypeToScope(enum_type->di_type), "tag_field",
                    import->di_file, decl_node->line + 1,
                    tag_debug_size_in_bits,
                    tag_debug_align_in_bits,
                    tag_offset_in_bits,
                    0, tag_di_type);

            uint64_t union_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, enum_type->type_ref, 1);
            ZigLLVMDIType *union_member_di_type = ZigLLVMCreateDebugMemberType(g->dbuilder,
                    ZigLLVMTypeToScope(enum_type->di_type), "union_field",
                    import->di_file, decl_node->line + 1,
                    biggest_union_member_size_in_bits,
                    biggest_align_in_bits,
                    union_offset_in_bits,
                    0, union_di_type);

            // create debug type for root struct
            ZigLLVMDIType *di_root_members[] = {
                tag_member_di_type,
                union_member_di_type,
            };


            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, enum_type->type_ref);
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, enum_type->type_ref);
            ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
                    ZigLLVMFileToScope(import->di_file),
                    buf_ptr(&enum_type->name),
                    import->di_file, decl_node->line + 1,
                    debug_size_in_bits,
                    debug_align_in_bits,
                    0, nullptr, di_root_members, 2, 0, nullptr, "");

            ZigLLVMReplaceTemporary(g->dbuilder, enum_type->di_type, replacement_di_type);
            enum_type->di_type = replacement_di_type;
        } else {
            // create llvm type for root struct
            enum_type->type_ref = tag_type_entry->type_ref;

            // create debug type for tag
            uint64_t tag_debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, tag_type_entry->type_ref);
            uint64_t tag_debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, tag_type_entry->type_ref);
            ZigLLVMDIType *tag_di_type = ZigLLVMCreateDebugEnumerationType(g->dbuilder,
                    ZigLLVMFileToScope(import->di_file), buf_ptr(&enum_type->name),
                    import->di_file, decl_node->line + 1,
                    tag_debug_size_in_bits,
                    tag_debug_align_in_bits,
                    di_enumerators, field_count,
                    tag_type_entry->di_type, "");

            ZigLLVMReplaceTemporary(g->dbuilder, enum_type->di_type, tag_di_type);
            enum_type->di_type = tag_di_type;
        }
    }
}

static void resolve_struct_type(CodeGen *g, TypeTableEntry *struct_type) {
    // if you change the logic of this function likely you must make a similar change in
    // parseh.cpp
    assert(struct_type->id == TypeTableEntryIdStruct);

    resolve_struct_zero_bits(g, struct_type);
    if (struct_type->data.structure.is_invalid)
        return;

    AstNode *decl_node = struct_type->data.structure.decl_node;

    if (struct_type->data.structure.embedded_in_current) {
        struct_type->data.structure.is_invalid = true;
        if (!struct_type->data.structure.reported_infinite_err) {
            struct_type->data.structure.reported_infinite_err = true;
            add_node_error(g, decl_node,
                    buf_sprintf("struct '%s' contains itself", buf_ptr(&struct_type->name)));
        }
        return;
    }

    assert(!struct_type->data.structure.zero_bits_loop_flag);
    assert(struct_type->data.enumeration.fields);
    assert(decl_node->type == NodeTypeContainerDecl);

    size_t field_count = struct_type->data.structure.src_field_count;

    size_t gen_field_count = struct_type->data.structure.gen_field_count;
    LLVMTypeRef *element_types = allocate<LLVMTypeRef>(gen_field_count);

    // this field should be set to true only during the recursive calls to resolve_struct_type
    struct_type->data.structure.embedded_in_current = true;

    Scope *scope = &struct_type->data.structure.decls_scope->base;

    for (size_t i = 0; i < field_count; i += 1) {
        TypeStructField *type_struct_field = &struct_type->data.structure.fields[i];
        TypeTableEntry *field_type = type_struct_field->type_entry;

        ensure_complete_type(g, field_type);
        if (field_type->id == TypeTableEntryIdInvalid) {
            struct_type->data.enumeration.is_invalid = true;
            continue;
        }

        struct_type->size_depends_on_compile_var = struct_type->size_depends_on_compile_var ||
            field_type->size_depends_on_compile_var;

        if (!type_has_bits(field_type))
            continue;

        element_types[type_struct_field->gen_index] = field_type->type_ref;
        assert(element_types[type_struct_field->gen_index]);
    }
    struct_type->data.structure.embedded_in_current = false;
    struct_type->data.structure.complete = true;

    if (struct_type->data.structure.is_invalid)
        return;

    if (struct_type->zero_bits) {
        struct_type->type_ref = LLVMVoidType();
        struct_type->di_type = g->builtin_types.entry_void->di_type;
        return;
    }
    assert(struct_type->di_type);

    LLVMStructSetBody(struct_type->type_ref, element_types, gen_field_count, false);

    ZigLLVMDIType **di_element_types = allocate<ZigLLVMDIType*>(gen_field_count);

    ImportTableEntry *import = get_scope_import(scope);
    for (size_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        TypeStructField *type_struct_field = &struct_type->data.structure.fields[i];
        size_t gen_field_index = type_struct_field->gen_index;
        if (gen_field_index == SIZE_MAX) {
            continue;
        }

        TypeTableEntry *field_type = type_struct_field->type_entry;

        // if the field is a function, actually the debug info should be a pointer.
        ZigLLVMDIType *field_di_type;
        if (field_type->id == TypeTableEntryIdFn) {
            TypeTableEntry *field_ptr_type = get_pointer_to_type(g, field_type, true);
            uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, field_ptr_type->type_ref);
            uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, field_ptr_type->type_ref);
            field_di_type = ZigLLVMCreateDebugPointerType(g->dbuilder, field_type->di_type,
                    debug_size_in_bits, debug_align_in_bits, buf_ptr(&field_ptr_type->name));
        } else {
            field_di_type = field_type->di_type;
        }

        assert(field_type->type_ref);
        assert(struct_type->type_ref);
        assert(struct_type->data.structure.complete);
        uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, field_type->type_ref);
        uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, field_type->type_ref);
        uint64_t debug_offset_in_bits = 8*LLVMOffsetOfElement(g->target_data_ref, struct_type->type_ref,
                gen_field_index);
        di_element_types[gen_field_index] = ZigLLVMCreateDebugMemberType(g->dbuilder,
                ZigLLVMTypeToScope(struct_type->di_type), buf_ptr(type_struct_field->name),
                import->di_file, field_node->line + 1,
                debug_size_in_bits,
                debug_align_in_bits,
                debug_offset_in_bits,
                0, field_di_type);

        assert(di_element_types[gen_field_index]);
    }


    uint64_t debug_size_in_bits = 8*LLVMStoreSizeOfType(g->target_data_ref, struct_type->type_ref);
    uint64_t debug_align_in_bits = 8*LLVMABISizeOfType(g->target_data_ref, struct_type->type_ref);
    ZigLLVMDIType *replacement_di_type = ZigLLVMCreateDebugStructType(g->dbuilder,
            ZigLLVMFileToScope(import->di_file),
            buf_ptr(&struct_type->name),
            import->di_file, decl_node->line + 1,
            debug_size_in_bits,
            debug_align_in_bits,
            0, nullptr, di_element_types, gen_field_count, 0, nullptr, "");

    ZigLLVMReplaceTemporary(g->dbuilder, struct_type->di_type, replacement_di_type);
    struct_type->di_type = replacement_di_type;
}

static void resolve_union_type(CodeGen *g, TypeTableEntry *union_type) {
    zig_panic("TODO");
}

static void resolve_enum_zero_bits(CodeGen *g, TypeTableEntry *enum_type) {
    assert(enum_type->id == TypeTableEntryIdEnum);

    if (enum_type->data.enumeration.zero_bits_known)
        return;

    if (enum_type->data.enumeration.zero_bits_loop_flag) {
        enum_type->data.enumeration.zero_bits_known = true;
        return;
    }

    enum_type->data.enumeration.zero_bits_loop_flag = true;

    AstNode *decl_node = enum_type->data.enumeration.decl_node;
    assert(decl_node->type == NodeTypeContainerDecl);
    assert(enum_type->di_type);

    assert(!enum_type->data.enumeration.fields);
    uint32_t field_count = decl_node->data.container_decl.fields.length;
    enum_type->data.enumeration.src_field_count = field_count;
    enum_type->data.enumeration.fields = allocate<TypeEnumField>(field_count);

    Scope *scope = &enum_type->data.enumeration.decls_scope->base;

    uint32_t gen_field_index = 0;
    for (uint32_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        TypeEnumField *type_enum_field = &enum_type->data.enumeration.fields[i];
        type_enum_field->name = field_node->data.struct_field.name;
        TypeTableEntry *field_type = analyze_type_expr(g, scope, field_node->data.struct_field.type);
        type_enum_field->type_entry = field_type;
        type_enum_field->value = i;

        type_ensure_zero_bits_known(g, field_type);
        if (field_type->id == TypeTableEntryIdInvalid) {
            enum_type->data.enumeration.is_invalid = true;
            continue;
        }

        if (!type_has_bits(field_type))
            continue;

        type_enum_field->gen_index = gen_field_index;
        gen_field_index += 1;
    }

    enum_type->data.enumeration.zero_bits_loop_flag = false;
    enum_type->data.enumeration.gen_field_count = gen_field_index;
    enum_type->zero_bits = (gen_field_index == 0 && field_count < 2);
    enum_type->data.enumeration.zero_bits_known = true;
}

static void resolve_struct_zero_bits(CodeGen *g, TypeTableEntry *struct_type) {
    assert(struct_type->id == TypeTableEntryIdStruct);

    if (struct_type->data.structure.zero_bits_known)
        return;

    if (struct_type->data.structure.zero_bits_loop_flag) {
        struct_type->data.structure.zero_bits_known = true;
        return;
    }

    struct_type->data.structure.zero_bits_loop_flag = true;

    AstNode *decl_node = struct_type->data.structure.decl_node;
    assert(decl_node->type == NodeTypeContainerDecl);
    assert(struct_type->di_type);

    assert(!struct_type->data.structure.fields);
    size_t field_count = decl_node->data.container_decl.fields.length;
    struct_type->data.structure.src_field_count = field_count;
    struct_type->data.structure.fields = allocate<TypeStructField>(field_count);

    Scope *scope = &struct_type->data.structure.decls_scope->base;

    size_t gen_field_index = 0;
    for (size_t i = 0; i < field_count; i += 1) {
        AstNode *field_node = decl_node->data.container_decl.fields.at(i);
        TypeStructField *type_struct_field = &struct_type->data.structure.fields[i];
        type_struct_field->name = field_node->data.struct_field.name;
        TypeTableEntry *field_type = analyze_type_expr(g, scope, field_node->data.struct_field.type);
        type_struct_field->type_entry = field_type;
        type_struct_field->src_index = i;
        type_struct_field->gen_index = SIZE_MAX;

        type_ensure_zero_bits_known(g, field_type);
        if (field_type->id == TypeTableEntryIdInvalid) {
            struct_type->data.structure.is_invalid = true;
            continue;
        }

        if (!type_has_bits(field_type))
            continue;

        type_struct_field->gen_index = gen_field_index;
        gen_field_index += 1;
    }

    struct_type->data.structure.zero_bits_loop_flag = false;
    struct_type->data.structure.gen_field_count = gen_field_index;
    struct_type->zero_bits = (gen_field_index == 0);
    struct_type->data.structure.zero_bits_known = true;
}

static void resolve_union_zero_bits(CodeGen *g, TypeTableEntry *union_type) {
    zig_panic("TODO resolve_union_zero_bits");
}

static void get_fully_qualified_decl_name_internal(Buf *buf, Scope *scope, uint8_t sep) {
    if (!scope)
        return;

    if (scope->id == ScopeIdDecls) {
        get_fully_qualified_decl_name_internal(buf, scope->parent, sep);

        ScopeDecls *scope_decls = (ScopeDecls *)scope;
        if (scope_decls->container_type) {
            buf_append_buf(buf, &scope_decls->container_type->name);
            buf_append_char(buf, sep);
        }
        return;
    }

    get_fully_qualified_decl_name_internal(buf, scope->parent, sep);
}

static void get_fully_qualified_decl_name(Buf *buf, Tld *tld, uint8_t sep) {
    buf_resize(buf, 0);
    get_fully_qualified_decl_name_internal(buf, tld->parent_scope, sep);
    buf_append_buf(buf, tld->name);
}

FnTableEntry *create_fn_raw(FnInline inline_value, bool internal_linkage) {
    FnTableEntry *fn_entry = allocate<FnTableEntry>(1);

    fn_entry->analyzed_executable.backward_branch_count = &fn_entry->prealloc_bbc;
    fn_entry->analyzed_executable.backward_branch_quota = default_backward_branch_quota;
    fn_entry->analyzed_executable.fn_entry = fn_entry;
    fn_entry->ir_executable.fn_entry = fn_entry;
    fn_entry->fn_inline = inline_value;
    fn_entry->internal_linkage = internal_linkage;

    return fn_entry;
}

FnTableEntry *create_fn(AstNode *proto_node) {
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    FnInline inline_value = fn_proto->is_inline ? FnInlineAlways : FnInlineAuto;
    bool internal_linkage = (fn_proto->visib_mod != VisibModExport && !proto_node->data.fn_proto.is_extern);
    FnTableEntry *fn_entry = create_fn_raw(inline_value, internal_linkage);

    fn_entry->proto_node = proto_node;
    fn_entry->fn_def_node = proto_node->data.fn_proto.fn_def_node;

    return fn_entry;
}

static bool scope_is_root_decls(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *scope_decls = (ScopeDecls *)scope;
            return (scope_decls->container_type == nullptr);
        }
        scope = scope->parent;
    }
    zig_unreachable();
}

static void resolve_decl_fn(CodeGen *g, TldFn *tld_fn) {
    ImportTableEntry *import = tld_fn->base.import;
    AstNode *proto_node = tld_fn->base.source_node;
    assert(proto_node->type == NodeTypeFnProto);
    AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

    AstNode *fn_def_node = fn_proto->fn_def_node;

    if (fn_def_node && fn_proto->is_var_args) {
        add_node_error(g, proto_node,
                buf_sprintf("variadic arguments only allowed in extern function declarations"));
        tld_fn->base.resolution = TldResolutionInvalid;
        return;
    }

    FnTableEntry *fn_table_entry = create_fn(tld_fn->base.source_node);
    get_fully_qualified_decl_name(&fn_table_entry->symbol_name, &tld_fn->base, '_');

    tld_fn->fn_entry = fn_table_entry;

    if (fn_table_entry->fn_def_node) {
        fn_table_entry->fndef_scope = create_fndef_scope(
            fn_table_entry->fn_def_node, tld_fn->base.parent_scope, fn_table_entry);

        for (size_t i = 0; i < fn_proto->params.length; i += 1) {
            AstNode *param_node = fn_proto->params.at(i);
            assert(param_node->type == NodeTypeParamDecl);
            if (buf_len(param_node->data.param_decl.name) == 0) {
                add_node_error(g, param_node, buf_sprintf("missing parameter name"));
            }
        }
    }

    Scope *child_scope = fn_table_entry->fndef_scope ? &fn_table_entry->fndef_scope->base : tld_fn->base.parent_scope;
    fn_table_entry->type_entry = analyze_fn_type(g, proto_node, child_scope);

    if (fn_table_entry->type_entry->id == TypeTableEntryIdInvalid) {
        tld_fn->base.resolution = TldResolutionInvalid;
        return;
    }

    if (!fn_table_entry->type_entry->data.fn.is_generic) {
        g->fn_protos.append(fn_table_entry);

        if (fn_def_node)
            g->fn_defs.append(fn_table_entry);

        bool is_main_fn = scope_is_root_decls(tld_fn->base.parent_scope) &&
            (import == g->root_import) && buf_eql_str(&fn_table_entry->symbol_name, "main");
        if (is_main_fn)
            g->main_fn = fn_table_entry;

        if (is_main_fn && !g->link_libc && tld_fn->base.visib_mod != VisibModExport) {
            TypeTableEntry *err_void = get_error_type(g, g->builtin_types.entry_void);
            TypeTableEntry *actual_return_type = fn_table_entry->type_entry->data.fn.fn_type_id.return_type;
            if (actual_return_type != err_void) {
                add_node_error(g, fn_proto->return_type,
                        buf_sprintf("expected return type of main to be '%%void', instead is '%s'",
                            buf_ptr(&actual_return_type->name)));
            }
        }
    }
}

static void add_top_level_decl(CodeGen *g, ScopeDecls *decls_scope, Tld *tld) {
    bool want_to_resolve = (g->check_unused || g->is_test_build || tld->visib_mod == VisibModExport);
    if (want_to_resolve)
        g->resolve_queue.append(tld);

    auto entry = decls_scope->decl_table.put_unique(tld->name, tld);
    if (entry) {
        Tld *other_tld = entry->value;
        ErrorMsg *msg = add_node_error(g, tld->source_node, buf_sprintf("redefinition of '%s'", buf_ptr(tld->name)));
        add_error_note(g, msg, other_tld->source_node, buf_sprintf("previous definition is here"));
        return;
    }
}

static void preview_error_value_decl(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeErrorValueDecl);

    if (node->data.error_value_decl.visib_mod != VisibModPrivate) {
        add_node_error(g, node, buf_sprintf("error values require no visibility modifier"));
    }

    ErrorTableEntry *err = allocate<ErrorTableEntry>(1);

    err->decl_node = node;
    buf_init_from_buf(&err->name, node->data.error_value_decl.name);

    auto existing_entry = g->error_table.maybe_get(&err->name);
    if (existing_entry) {
        // duplicate error definitions allowed and they get the same value
        err->value = existing_entry->value->value;
    } else {
        size_t error_value_count = g->error_decls.length;
        assert((uint32_t)error_value_count < (((uint32_t)1) << (uint32_t)g->err_tag_type->data.integral.bit_count));
        err->value = error_value_count;
        g->error_decls.append(node);
        g->error_table.put(&err->name, err);
    }

    node->data.error_value_decl.err = err;
}

void init_tld(Tld *tld, TldId id, Buf *name, VisibMod visib_mod, AstNode *source_node,
    Scope *parent_scope)
{
    tld->id = id;
    tld->name = name;
    tld->visib_mod = visib_mod;
    tld->source_node = source_node;
    tld->import = source_node->owner;
    tld->parent_scope = parent_scope;
}

void scan_decls(CodeGen *g, ScopeDecls *decls_scope, AstNode *node) {
    switch (node->type) {
        case NodeTypeRoot:
            for (size_t i = 0; i < node->data.root.top_level_decls.length; i += 1) {
                AstNode *child = node->data.root.top_level_decls.at(i);
                scan_decls(g, decls_scope, child);
            }
            break;
        case NodeTypeFnDef:
            scan_decls(g, decls_scope, node->data.fn_def.fn_proto);
            break;
        case NodeTypeVariableDeclaration:
            {
                Buf *name = node->data.variable_declaration.symbol;
                VisibMod visib_mod = node->data.variable_declaration.visib_mod;
                TldVar *tld_var = allocate<TldVar>(1);
                init_tld(&tld_var->base, TldIdVar, name, visib_mod, node, &decls_scope->base);
                add_top_level_decl(g, decls_scope, &tld_var->base);
                break;
            }
        case NodeTypeTypeDecl:
            {
                Buf *name = node->data.type_decl.symbol;
                VisibMod visib_mod = node->data.type_decl.visib_mod;
                TldTypeDef *tld_typedef = allocate<TldTypeDef>(1);
                init_tld(&tld_typedef->base, TldIdTypeDef, name, visib_mod, node, &decls_scope->base);
                add_top_level_decl(g, decls_scope, &tld_typedef->base);
                break;
            }
        case NodeTypeFnProto:
            {
                // if the name is missing, we immediately announce an error
                Buf *fn_name = node->data.fn_proto.name;
                if (buf_len(fn_name) == 0) {
                    add_node_error(g, node, buf_sprintf("missing function name"));
                    break;
                }

                VisibMod visib_mod = node->data.fn_proto.visib_mod;
                TldFn *tld_fn = allocate<TldFn>(1);
                init_tld(&tld_fn->base, TldIdFn, fn_name, visib_mod, node, &decls_scope->base);
                add_top_level_decl(g, decls_scope, &tld_fn->base);
                break;
            }
        case NodeTypeUse:
            {
                g->use_queue.append(node);
                ImportTableEntry *import = get_scope_import(&decls_scope->base);
                import->use_decls.append(node);
                break;
            }
        case NodeTypeErrorValueDecl:
            // error value declarations do not depend on other top level decls
            preview_error_value_decl(g, node);
            break;
        case NodeTypeContainerDecl:
        case NodeTypeParamDecl:
        case NodeTypeFnDecl:
        case NodeTypeReturnExpr:
        case NodeTypeDefer:
        case NodeTypeBlock:
        case NodeTypeBinOpExpr:
        case NodeTypeUnwrapErrorExpr:
        case NodeTypeFnCallExpr:
        case NodeTypeArrayAccessExpr:
        case NodeTypeSliceExpr:
        case NodeTypeNumberLiteral:
        case NodeTypeStringLiteral:
        case NodeTypeCharLiteral:
        case NodeTypeBoolLiteral:
        case NodeTypeNullLiteral:
        case NodeTypeUndefinedLiteral:
        case NodeTypeZeroesLiteral:
        case NodeTypeThisLiteral:
        case NodeTypeSymbol:
        case NodeTypePrefixOpExpr:
        case NodeTypeIfBoolExpr:
        case NodeTypeIfVarExpr:
        case NodeTypeWhileExpr:
        case NodeTypeForExpr:
        case NodeTypeSwitchExpr:
        case NodeTypeSwitchProng:
        case NodeTypeSwitchRange:
        case NodeTypeLabel:
        case NodeTypeGoto:
        case NodeTypeBreak:
        case NodeTypeContinue:
        case NodeTypeAsmExpr:
        case NodeTypeFieldAccessExpr:
        case NodeTypeStructField:
        case NodeTypeContainerInitExpr:
        case NodeTypeStructValueField:
        case NodeTypeArrayType:
        case NodeTypeErrorType:
        case NodeTypeTypeLiteral:
        case NodeTypeVarLiteral:
            zig_unreachable();
    }
}

static void resolve_decl_container(CodeGen *g, TldContainer *tld_container) {
    TypeTableEntry *type_entry = tld_container->type_entry;
    assert(type_entry);

    switch (type_entry->id) {
        case TypeTableEntryIdStruct:
            resolve_struct_type(g, tld_container->type_entry);
            return;
        case TypeTableEntryIdEnum:
            resolve_enum_type(g, tld_container->type_entry);
            return;
        case TypeTableEntryIdUnion:
            resolve_union_type(g, tld_container->type_entry);
            return;
        default:
            zig_unreachable();
    }
}

TypeTableEntry *validate_var_type(CodeGen *g, AstNode *source_node, TypeTableEntry *type_entry) {
    TypeTableEntry *underlying_type = get_underlying_type(type_entry);
    switch (underlying_type->id) {
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdInvalid:
            return g->builtin_types.entry_invalid;
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdBlock:
            add_node_error(g, source_node, buf_sprintf("variable of type '%s' not allowed",
                buf_ptr(&underlying_type->name)));
            return g->builtin_types.entry_invalid;
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdEnumTag:
            return type_entry;
    }
    zig_unreachable();
}

// Set name to nullptr to make the variable anonymous (not visible to programmer).
// TODO merge with definition of add_local_var in ir.cpp
VariableTableEntry *add_variable(CodeGen *g, AstNode *source_node, Scope *parent_scope, Buf *name,
    bool is_const, ConstExprValue *value)
{
    assert(value);

    VariableTableEntry *variable_entry = allocate<VariableTableEntry>(1);
    variable_entry->value = *value;
    variable_entry->parent_scope = parent_scope;
    variable_entry->shadowable = false;
    variable_entry->mem_slot_index = SIZE_MAX;
    variable_entry->src_arg_index = SIZE_MAX;

    assert(name);

    buf_init_from_buf(&variable_entry->name, name);

    if (value->type->id != TypeTableEntryIdInvalid) {
        VariableTableEntry *existing_var = find_variable(g, parent_scope, name);
        if (existing_var && !existing_var->shadowable) {
            ErrorMsg *msg = add_node_error(g, source_node,
                    buf_sprintf("redeclaration of variable '%s'", buf_ptr(name)));
            add_error_note(g, msg, existing_var->decl_node, buf_sprintf("previous declaration is here"));
            variable_entry->value.type = g->builtin_types.entry_invalid;
        } else {
            auto primitive_table_entry = g->primitive_type_table.maybe_get(name);
            if (primitive_table_entry) {
                TypeTableEntry *type = primitive_table_entry->value;
                add_node_error(g, source_node,
                        buf_sprintf("variable shadows type '%s'", buf_ptr(&type->name)));
                variable_entry->value.type = g->builtin_types.entry_invalid;
            } else {
                Tld *tld = find_decl(parent_scope, name);
                if (tld && tld->id != TldIdVar) {
                    ErrorMsg *msg = add_node_error(g, source_node,
                            buf_sprintf("redefinition of '%s'", buf_ptr(name)));
                    add_error_note(g, msg, tld->source_node, buf_sprintf("previous definition is here"));
                    variable_entry->value.type = g->builtin_types.entry_invalid;
                }
            }
        }
    }

    Scope *child_scope;
    if (source_node->type == NodeTypeParamDecl) {
        child_scope = create_var_scope(source_node, parent_scope, variable_entry);
    } else {
        // it's already in the decls table
        child_scope = parent_scope;
    }


    variable_entry->src_is_const = is_const;
    variable_entry->gen_is_const = is_const;
    variable_entry->decl_node = source_node;
    variable_entry->child_scope = child_scope;


    return variable_entry;
}

static void resolve_decl_var(CodeGen *g, TldVar *tld_var) {
    AstNodeVariableDeclaration *var_decl = &tld_var->base.source_node->data.variable_declaration;

    bool is_const = var_decl->is_const;
    bool is_export = (tld_var->base.visib_mod == VisibModExport);
    bool is_extern = var_decl->is_extern;

    TypeTableEntry *explicit_type = nullptr;
    if (var_decl->type) {
        TypeTableEntry *proposed_type = analyze_type_expr(g, tld_var->base.parent_scope, var_decl->type);
        explicit_type = validate_var_type(g, var_decl->type, proposed_type);
    }

    AstNode *source_node = tld_var->base.source_node;

    IrInstruction *init_value = nullptr;

    TypeTableEntry *implicit_type = nullptr;
    if (explicit_type && explicit_type->id == TypeTableEntryIdInvalid) {
        implicit_type = explicit_type;
    } else if (var_decl->expr) {
        init_value = analyze_const_value(g, tld_var->base.parent_scope, var_decl->expr, explicit_type, var_decl->symbol);
        assert(init_value);
        implicit_type = init_value->value.type;

        if (implicit_type->id == TypeTableEntryIdUnreachable) {
            add_node_error(g, source_node, buf_sprintf("variable initialization is unreachable"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if ((!is_const || is_export) &&
                (implicit_type->id == TypeTableEntryIdNumLitFloat ||
                implicit_type->id == TypeTableEntryIdNumLitInt))
        {
            add_node_error(g, source_node, buf_sprintf("unable to infer variable type"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if (implicit_type->id == TypeTableEntryIdNullLit) {
            add_node_error(g, source_node, buf_sprintf("unable to infer variable type"));
            implicit_type = g->builtin_types.entry_invalid;
        } else if (implicit_type->id == TypeTableEntryIdMetaType && !is_const) {
            add_node_error(g, source_node, buf_sprintf("variable of type 'type' must be constant"));
            implicit_type = g->builtin_types.entry_invalid;
        }
        assert(implicit_type->id == TypeTableEntryIdInvalid || init_value->value.special != ConstValSpecialRuntime);
    } else if (!is_extern) {
        add_node_error(g, source_node, buf_sprintf("variables must be initialized"));
        implicit_type = g->builtin_types.entry_invalid;
    }

    TypeTableEntry *type = explicit_type ? explicit_type : implicit_type;
    assert(type != nullptr); // should have been caught by the parser

    ConstExprValue *init_val = init_value ? &init_value->value : create_const_runtime(type);

    tld_var->var = add_variable(g, source_node, tld_var->base.parent_scope, var_decl->symbol, is_const, init_val);
    tld_var->var->is_extern = is_extern;

    g->global_vars.append(tld_var->var);
}

static void resolve_decl_typedef(CodeGen *g, TldTypeDef *tld_typedef) {
    AstNode *typedef_node = tld_typedef->base.source_node;
    assert(typedef_node->type == NodeTypeTypeDecl);
    AstNode *type_node = typedef_node->data.type_decl.child_type;
    Buf *decl_name = typedef_node->data.type_decl.symbol;

    TypeTableEntry *child_type = analyze_type_expr(g, tld_typedef->base.parent_scope, type_node);
    tld_typedef->type_entry = (child_type->id == TypeTableEntryIdInvalid) ?
        child_type : get_typedecl_type(g, buf_ptr(decl_name), child_type);
}

void resolve_top_level_decl(CodeGen *g, Tld *tld, bool pointer_only) {
    if (tld->resolution != TldResolutionUnresolved)
        return;

    if (tld->dep_loop_flag) {
        add_node_error(g, tld->source_node, buf_sprintf("'%s' depends on itself", buf_ptr(tld->name)));
        tld->resolution = TldResolutionInvalid;
        return;
    } else {
        tld->dep_loop_flag = true;
    }

    switch (tld->id) {
        case TldIdVar:
            {
                TldVar *tld_var = (TldVar *)tld;
                resolve_decl_var(g, tld_var);
                break;
            }
        case TldIdFn:
            {
                TldFn *tld_fn = (TldFn *)tld;
                resolve_decl_fn(g, tld_fn);
                break;
            }
        case TldIdContainer:
            {
                TldContainer *tld_container = (TldContainer *)tld;
                resolve_decl_container(g, tld_container);
                break;
            }
        case TldIdTypeDef:
            {
                TldTypeDef *tld_typedef = (TldTypeDef *)tld;
                resolve_decl_typedef(g, tld_typedef);
                break;
            }
    }

    tld->resolution = TldResolutionOk;
    tld->dep_loop_flag = false;
}

static bool type_has_codegen_value(TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
            return false;

        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdEnumTag:
            return true;

        case TypeTableEntryIdTypeDecl:
            return type_has_codegen_value(type_entry->data.type_decl.canonical_type);

        case TypeTableEntryIdVar:
            zig_unreachable();
    }
    zig_unreachable();
}

bool types_match_const_cast_only(TypeTableEntry *expected_type, TypeTableEntry *actual_type) {
    if (expected_type == actual_type)
        return true;

    // pointer const
    if (expected_type->id == TypeTableEntryIdPointer &&
        actual_type->id == TypeTableEntryIdPointer &&
        (!actual_type->data.pointer.is_const || expected_type->data.pointer.is_const))
    {
        return types_match_const_cast_only(expected_type->data.pointer.child_type,
                actual_type->data.pointer.child_type);
    }

    // unknown size array const
    if (expected_type->id == TypeTableEntryIdStruct &&
        actual_type->id == TypeTableEntryIdStruct &&
        expected_type->data.structure.is_slice &&
        actual_type->data.structure.is_slice &&
        (!actual_type->data.structure.fields[0].type_entry->data.pointer.is_const ||
          expected_type->data.structure.fields[0].type_entry->data.pointer.is_const))
    {
        return types_match_const_cast_only(
                expected_type->data.structure.fields[0].type_entry->data.pointer.child_type,
                actual_type->data.structure.fields[0].type_entry->data.pointer.child_type);
    }

    // maybe
    if (expected_type->id == TypeTableEntryIdMaybe &&
        actual_type->id == TypeTableEntryIdMaybe)
    {
        return types_match_const_cast_only(
                expected_type->data.maybe.child_type,
                actual_type->data.maybe.child_type);
    }

    // error
    if (expected_type->id == TypeTableEntryIdErrorUnion &&
        actual_type->id == TypeTableEntryIdErrorUnion)
    {
        return types_match_const_cast_only(
                expected_type->data.error.child_type,
                actual_type->data.error.child_type);
    }

    // fn
    if (expected_type->id == TypeTableEntryIdFn &&
        actual_type->id == TypeTableEntryIdFn)
    {
        if (expected_type->data.fn.fn_type_id.is_extern != actual_type->data.fn.fn_type_id.is_extern) {
            return false;
        }
        if (expected_type->data.fn.fn_type_id.is_naked != actual_type->data.fn.fn_type_id.is_naked) {
            return false;
        }
        if (expected_type->data.fn.fn_type_id.is_cold != actual_type->data.fn.fn_type_id.is_cold) {
            return false;
        }
        if (actual_type->data.fn.fn_type_id.return_type->id != TypeTableEntryIdUnreachable &&
            !types_match_const_cast_only(
                expected_type->data.fn.fn_type_id.return_type,
                actual_type->data.fn.fn_type_id.return_type))
        {
            return false;
        }
        if (expected_type->data.fn.fn_type_id.param_count != actual_type->data.fn.fn_type_id.param_count) {
            return false;
        }
        for (size_t i = 0; i < expected_type->data.fn.fn_type_id.param_count; i += 1) {
            // note it's reversed for parameters
            FnTypeParamInfo *actual_param_info = &actual_type->data.fn.fn_type_id.param_info[i];
            FnTypeParamInfo *expected_param_info = &expected_type->data.fn.fn_type_id.param_info[i];

            if (!types_match_const_cast_only(actual_param_info->type, expected_param_info->type)) {
                return false;
            }

            if (expected_param_info->is_noalias != actual_param_info->is_noalias) {
                return false;
            }
        }
        return true;
    }


    return false;
}

Tld *find_decl(Scope *scope, Buf *name) {
    while (scope) {
        if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;
            auto entry = decls_scope->decl_table.maybe_get(name);
            if (entry)
                return entry->value;
        }
        scope = scope->parent;
    }
    return nullptr;
}

VariableTableEntry *find_variable(CodeGen *g, Scope *scope, Buf *name) {
    while (scope) {
        if (scope->id == ScopeIdVarDecl) {
            ScopeVarDecl *var_scope = (ScopeVarDecl *)scope;
            if (buf_eql_buf(name, &var_scope->var->name))
                return var_scope->var;
        } else if (scope->id == ScopeIdDecls) {
            ScopeDecls *decls_scope = (ScopeDecls *)scope;
            auto entry = decls_scope->decl_table.maybe_get(name);
            if (entry) {
                Tld *tld = entry->value;
                if (tld->id == TldIdVar) {
                    TldVar *tld_var = (TldVar *)tld;
                    if (tld_var->var)
                        return tld_var->var;
                }
            }
        }
        scope = scope->parent;
    }

    return nullptr;
}

FnTableEntry *scope_fn_entry(Scope *scope) {
    while (scope) {
        if (scope->id == ScopeIdFnDef) {
            ScopeFnDef *fn_scope = (ScopeFnDef *)scope;
            return fn_scope->fn_entry;
        }
        scope = scope->parent;
    }
    return nullptr;
}

FnTableEntry *scope_get_fn_if_root(Scope *scope) {
    assert(scope);
    scope = scope->parent;
    while (scope) {
        switch (scope->id) {
            case ScopeIdBlock:
                return nullptr;
            case ScopeIdDecls:
            case ScopeIdDefer:
            case ScopeIdDeferExpr:
            case ScopeIdVarDecl:
            case ScopeIdCImport:
            case ScopeIdLoop:
                scope = scope->parent;
                continue;
            case ScopeIdFnDef:
                ScopeFnDef *fn_scope = (ScopeFnDef *)scope;
                return fn_scope->fn_entry;
        }
        zig_unreachable();
    }
    return nullptr;
}

TypeEnumField *find_enum_type_field(TypeTableEntry *enum_type, Buf *name) {
    for (uint32_t i = 0; i < enum_type->data.enumeration.src_field_count; i += 1) {
        TypeEnumField *type_enum_field = &enum_type->data.enumeration.fields[i];
        if (buf_eql_buf(type_enum_field->name, name)) {
            return type_enum_field;
        }
    }
    return nullptr;
}

TypeStructField *find_struct_type_field(TypeTableEntry *type_entry, Buf *name) {
    assert(type_entry->id == TypeTableEntryIdStruct);
    assert(type_entry->data.structure.complete);
    for (uint32_t i = 0; i < type_entry->data.structure.src_field_count; i += 1) {
        TypeStructField *field = &type_entry->data.structure.fields[i];
        if (buf_eql_buf(field->name, name)) {
            return field;
        }
    }
    return nullptr;
}

static bool is_container(TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdVar:
            zig_unreachable();
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdUnion:
            return true;
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdEnumTag:
            return false;
    }
    zig_unreachable();
}

bool is_container_ref(TypeTableEntry *type_entry) {
    return (type_entry->id == TypeTableEntryIdPointer) ?
        is_container(type_entry->data.pointer.child_type) : is_container(type_entry);
}

TypeTableEntry *container_ref_type(TypeTableEntry *type_entry) {
    assert(is_container_ref(type_entry));
    return (type_entry->id == TypeTableEntryIdPointer) ?
        type_entry->data.pointer.child_type : type_entry;
}

void resolve_container_type(CodeGen *g, TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdStruct:
            resolve_struct_type(g, type_entry);
            break;
        case TypeTableEntryIdEnum:
            resolve_enum_type(g, type_entry);
            break;
        case TypeTableEntryIdUnion:
            resolve_union_type(g, type_entry);
            break;
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdArray:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdTypeDecl:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdVar:
        case TypeTableEntryIdEnumTag:
            zig_unreachable();
    }
}

bool type_is_codegen_pointer(TypeTableEntry *type) {
    if (type->id == TypeTableEntryIdPointer) return true;
    if (type->id == TypeTableEntryIdFn) return true;
    if (type->id == TypeTableEntryIdMaybe) {
        if (type->data.maybe.child_type->id == TypeTableEntryIdPointer) return true;
        if (type->data.maybe.child_type->id == TypeTableEntryIdFn) return true;
    }
    return false;
}

AstNode *get_param_decl_node(FnTableEntry *fn_entry, size_t index) {
    if (fn_entry->param_source_nodes)
        return fn_entry->param_source_nodes[index];
    else
        return fn_entry->proto_node->data.fn_proto.params.at(index);
}

static void analyze_fn_body(CodeGen *g, FnTableEntry *fn_table_entry) {
    assert(fn_table_entry->anal_state != FnAnalStateProbing);
    if (fn_table_entry->anal_state != FnAnalStateReady)
        return;

    fn_table_entry->anal_state = FnAnalStateProbing;

    AstNodeFnProto *fn_proto = &fn_table_entry->proto_node->data.fn_proto;

    assert(fn_table_entry->fndef_scope);
    if (!fn_table_entry->child_scope)
        fn_table_entry->child_scope = &fn_table_entry->fndef_scope->base;

    // define local variables for parameters
    TypeTableEntry *fn_type = fn_table_entry->type_entry;
    assert(!fn_type->data.fn.is_generic);
    FnTypeId *fn_type_id = &fn_type->data.fn.fn_type_id;
    for (size_t i = 0; i < fn_type_id->param_count; i += 1) {
        FnTypeParamInfo *param_info = &fn_type_id->param_info[i];
        AstNode *param_decl_node = get_param_decl_node(fn_table_entry, i);
        AstNodeParamDecl *param_decl = &param_decl_node->data.param_decl;

        TypeTableEntry *param_type = param_info->type;
        bool is_noalias = param_info->is_noalias;

        if (is_noalias && !type_is_codegen_pointer(param_type)) {
            add_node_error(g, param_decl_node, buf_sprintf("noalias on non-pointer parameter"));
        }

        if (fn_type_id->is_extern && handle_is_ptr(param_type)) {
            add_node_error(g, param_decl_node,
                buf_sprintf("byvalue types not yet supported on extern function parameters"));
        }

        VariableTableEntry *var = add_variable(g, param_decl_node, fn_table_entry->child_scope,
                param_decl->name, true, create_const_runtime(param_type));
        var->src_arg_index = i;
        fn_table_entry->child_scope = var->child_scope;

        if (type_has_bits(param_type)) {
            fn_table_entry->variable_list.append(var);
        }

        if (fn_type->data.fn.gen_param_info) {
            var->gen_arg_index = fn_type->data.fn.gen_param_info[i].gen_index;
        }
    }

    TypeTableEntry *expected_type = fn_type_id->return_type;

    if (fn_type_id->is_extern && handle_is_ptr(expected_type)) {
        add_node_error(g, fn_proto->return_type,
            buf_sprintf("byvalue types not yet supported on extern function return values"));
    }

    ir_gen_fn(g, fn_table_entry);
    if (fn_table_entry->ir_executable.invalid) {
        fn_table_entry->anal_state = FnAnalStateInvalid;
        return;
    }
    if (g->verbose) {
        fprintf(stderr, "\n");
        ast_render(stderr, fn_table_entry->fn_def_node, 4);
        fprintf(stderr, "\n{ // (IR)\n");
        ir_print(stderr, &fn_table_entry->ir_executable, 4);
        fprintf(stderr, "}\n");
    }

    TypeTableEntry *block_return_type = ir_analyze(g, &fn_table_entry->ir_executable,
            &fn_table_entry->analyzed_executable, expected_type, fn_proto->return_type);
    fn_table_entry->implicit_return_type = block_return_type;

    if (block_return_type->id == TypeTableEntryIdInvalid ||
        fn_table_entry->analyzed_executable.invalid)
    {
        assert(g->errors.length > 0);
        fn_table_entry->anal_state = FnAnalStateInvalid;
        return;
    }

    if (g->verbose) {
        fprintf(stderr, "{ // (analyzed)\n");
        ir_print(stderr, &fn_table_entry->analyzed_executable, 4);
        fprintf(stderr, "}\n");
    }

    fn_table_entry->anal_state = FnAnalStateComplete;
}

static void add_symbols_from_import(CodeGen *g, AstNode *dst_use_node) {
    IrInstruction *use_target_value = dst_use_node->data.use.value;
    if (use_target_value->value.type->id == TypeTableEntryIdInvalid) {
        dst_use_node->owner->any_imports_failed = true;
        return;
    }

    dst_use_node->data.use.resolution = TldResolutionOk;

    ConstExprValue *const_val = &use_target_value->value;
    assert(const_val->special != ConstValSpecialRuntime);

    ImportTableEntry *target_import = const_val->data.x_import;
    assert(target_import);

    if (target_import->any_imports_failed) {
        dst_use_node->owner->any_imports_failed = true;
    }

    auto it = target_import->decls_scope->decl_table.entry_iterator();
    for (;;) {
        auto *entry = it.next();
        if (!entry)
            break;

        Tld *target_tld = entry->value;
        if (target_tld->import != target_import ||
            target_tld->visib_mod == VisibModPrivate)
        {
            continue;
        }

        auto existing_entry = dst_use_node->owner->decls_scope->decl_table.put_unique(target_tld->name, target_tld);
        if (existing_entry) {
            Tld *existing_decl = existing_entry->value;
            if (existing_decl != target_tld) {
                ErrorMsg *msg = add_node_error(g, dst_use_node,
                        buf_sprintf("import of '%s' overrides existing definition",
                            buf_ptr(target_tld->name)));
                add_error_note(g, msg, existing_decl->source_node, buf_sprintf("previous definition here"));
                add_error_note(g, msg, target_tld->source_node, buf_sprintf("imported definition here"));
            }
        }
    }

    for (size_t i = 0; i < target_import->use_decls.length; i += 1) {
        AstNode *use_decl_node = target_import->use_decls.at(i);
        if (use_decl_node->data.use.visib_mod != VisibModPrivate)
            add_symbols_from_import(g, dst_use_node);
    }
}

void resolve_use_decl(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeUse);

    if (node->data.use.resolution != TldResolutionUnresolved)
        return;
    add_symbols_from_import(g, node);
}

void preview_use_decl(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeUse);

    IrInstruction *result = analyze_const_value(g, &node->owner->decls_scope->base,
        node->data.use.expr, g->builtin_types.entry_namespace, nullptr);

    if (result->value.type->id == TypeTableEntryIdInvalid)
        node->owner->any_imports_failed = true;

    node->data.use.value = result;
}

ImportTableEntry *add_source_file(CodeGen *g, PackageTableEntry *package,
        Buf *abs_full_path, Buf *src_dirname, Buf *src_basename, Buf *source_code)
{
    Buf *full_path = buf_alloc();
    os_path_join(src_dirname, src_basename, full_path);

    if (g->verbose) {
        fprintf(stderr, "\nOriginal Source (%s):\n", buf_ptr(full_path));
        fprintf(stderr, "----------------\n");
        fprintf(stderr, "%s\n", buf_ptr(source_code));

        fprintf(stderr, "\nTokens:\n");
        fprintf(stderr, "---------\n");
    }

    Tokenization tokenization = {0};
    tokenize(source_code, &tokenization);

    if (tokenization.err) {
        ErrorMsg *err = err_msg_create_with_line(full_path, tokenization.err_line, tokenization.err_column,
                source_code, tokenization.line_offsets, tokenization.err);

        print_err_msg(err, g->err_color);
        exit(1);
    }

    if (g->verbose) {
        print_tokens(source_code, tokenization.tokens);

        fprintf(stderr, "\nAST:\n");
        fprintf(stderr, "------\n");
    }

    ImportTableEntry *import_entry = allocate<ImportTableEntry>(1);
    import_entry->package = package;
    import_entry->source_code = source_code;
    import_entry->line_offsets = tokenization.line_offsets;
    import_entry->path = full_path;

    import_entry->root = ast_parse(source_code, tokenization.tokens, import_entry, g->err_color,
            &g->next_node_index);
    assert(import_entry->root);
    if (g->verbose) {
        ast_print(stderr, import_entry->root, 0);
        //fprintf(stderr, "\nReformatted Source:\n");
        //fprintf(stderr, "---------------------\n");
        //ast_render(stderr, import_entry->root, 4);
    }

    import_entry->di_file = ZigLLVMCreateFile(g->dbuilder, buf_ptr(src_basename), buf_ptr(src_dirname));
    g->import_table.put(abs_full_path, import_entry);
    g->import_queue.append(import_entry);

    import_entry->decls_scope = create_decls_scope(import_entry->root, nullptr, nullptr, import_entry);


    assert(import_entry->root->type == NodeTypeRoot);
    for (size_t decl_i = 0; decl_i < import_entry->root->data.root.top_level_decls.length; decl_i += 1) {
        AstNode *top_level_decl = import_entry->root->data.root.top_level_decls.at(decl_i);

        if (top_level_decl->type == NodeTypeFnDef) {
            AstNode *proto_node = top_level_decl->data.fn_def.fn_proto;
            assert(proto_node->type == NodeTypeFnProto);
            Buf *proto_name = proto_node->data.fn_proto.name;

            bool is_pub = (proto_node->data.fn_proto.visib_mod == VisibModPub);

            if (buf_eql_str(proto_name, "main") && is_pub) {
                g->have_exported_main = true;
            }
        }
    }

    return import_entry;
}


void semantic_analyze(CodeGen *g) {
    for (; g->import_queue_index < g->import_queue.length; g->import_queue_index += 1) {
        ImportTableEntry *import = g->import_queue.at(g->import_queue_index);
        scan_decls(g, import->decls_scope, import->root);
    }

    for (; g->use_queue_index < g->use_queue.length; g->use_queue_index += 1) {
        AstNode *use_decl_node = g->use_queue.at(g->use_queue_index);
        preview_use_decl(g, use_decl_node);
    }

    for (size_t i = 0; i < g->use_queue.length; i += 1) {
        AstNode *use_decl_node = g->use_queue.at(i);
        resolve_use_decl(g, use_decl_node);
    }

    while (g->resolve_queue_index < g->resolve_queue.length ||
           g->fn_defs_index < g->fn_defs.length)
    {
        for (; g->resolve_queue_index < g->resolve_queue.length; g->resolve_queue_index += 1) {
            Tld *tld = g->resolve_queue.at(g->resolve_queue_index);
            bool pointer_only = false;
            resolve_top_level_decl(g, tld, pointer_only);
        }

        for (; g->fn_defs_index < g->fn_defs.length; g->fn_defs_index += 1) {
            FnTableEntry *fn_entry = g->fn_defs.at(g->fn_defs_index);
            analyze_fn_body(g, fn_entry);
        }
    }
}

bool is_node_void_expr(AstNode *node) {
    if (node->type == NodeTypeContainerInitExpr &&
        node->data.container_init_expr.kind == ContainerInitKindArray)
    {
        AstNode *type_node = node->data.container_init_expr.type;
        if (type_node->type == NodeTypeSymbol &&
            buf_eql_str(type_node->data.symbol_expr.symbol, "void"))
        {
            return true;
        }
    } else if (node->type == NodeTypeBlock && node->data.block.statements.length == 0) {
        return true;
    }

    return false;
}

TypeTableEntry **get_int_type_ptr(CodeGen *g, bool is_signed, size_t size_in_bits) {
    size_t index;
    if (size_in_bits == 8) {
        index = 0;
    } else if (size_in_bits == 16) {
        index = 1;
    } else if (size_in_bits == 32) {
        index = 2;
    } else if (size_in_bits == 64) {
        index = 3;
    } else {
        zig_unreachable();
    }
    return &g->builtin_types.entry_int[is_signed ? 0 : 1][index];
}

TypeTableEntry *get_int_type(CodeGen *g, bool is_signed, size_t size_in_bits) {
    return *get_int_type_ptr(g, is_signed, size_in_bits);
}

TypeTableEntry **get_c_int_type_ptr(CodeGen *g, CIntType c_int_type) {
    return &g->builtin_types.entry_c_int[c_int_type];
}

TypeTableEntry *get_c_int_type(CodeGen *g, CIntType c_int_type) {
    return *get_c_int_type_ptr(g, c_int_type);
}

bool handle_is_ptr(TypeTableEntry *type_entry) {
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdVar:
             zig_unreachable();
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdEnumTag:
             return false;
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdUnion:
             return true;
        case TypeTableEntryIdErrorUnion:
             return type_has_bits(type_entry->data.error.child_type);
        case TypeTableEntryIdEnum:
             assert(type_entry->data.enumeration.complete);
             return type_entry->data.enumeration.gen_field_count != 0;
        case TypeTableEntryIdMaybe:
             return type_entry->data.maybe.child_type->id != TypeTableEntryIdPointer &&
                    type_entry->data.maybe.child_type->id != TypeTableEntryIdFn;
        case TypeTableEntryIdTypeDecl:
             return handle_is_ptr(type_entry->data.type_decl.canonical_type);
    }
    zig_unreachable();
}

void find_libc_include_path(CodeGen *g) {
    if (!g->libc_include_dir || buf_len(g->libc_include_dir) == 0) {
        zig_panic("Unable to determine libc include path.");
    }
}

void find_libc_lib_path(CodeGen *g) {
    // later we can handle this better by reporting an error via the normal mechanism
    if (!g->libc_lib_dir || buf_len(g->libc_lib_dir) == 0) {
        zig_panic("Unable to determine libc lib path.");
    }
    if (!g->libc_static_lib_dir || buf_len(g->libc_static_lib_dir) == 0) {
        zig_panic("Unable to determine libc static lib path.");
    }
}

static uint32_t hash_ptr(void *ptr) {
    return ((uintptr_t)ptr) % UINT32_MAX;
}

static uint32_t hash_size(size_t x) {
    return x % UINT32_MAX;
}

uint32_t fn_table_entry_hash(FnTableEntry* value) {
    return ptr_hash(value);
}

bool fn_table_entry_eql(FnTableEntry *a, FnTableEntry *b) {
    return ptr_eq(a, b);
}

uint32_t fn_type_id_hash(FnTypeId *id) {
    uint32_t result = 0;
    result += id->is_extern ? 3349388391 : 0;
    result += id->is_naked ? 608688877 : 0;
    result += id->is_cold ? 3605523458 : 0;
    result += id->is_var_args ? 1931444534 : 0;
    result += hash_ptr(id->return_type);
    for (size_t i = 0; i < id->param_count; i += 1) {
        FnTypeParamInfo *info = &id->param_info[i];
        result += info->is_noalias ? 892356923 : 0;
        result += hash_ptr(info->type);
    }
    return result;
}

bool fn_type_id_eql(FnTypeId *a, FnTypeId *b) {
    if (a->is_extern != b->is_extern ||
        a->is_naked != b->is_naked ||
        a->is_cold != b->is_cold ||
        a->return_type != b->return_type ||
        a->is_var_args != b->is_var_args ||
        a->param_count != b->param_count)
    {
        return false;
    }
    for (size_t i = 0; i < a->param_count; i += 1) {
        FnTypeParamInfo *a_param_info = &a->param_info[i];
        FnTypeParamInfo *b_param_info = &b->param_info[i];

        if (a_param_info->type != b_param_info->type ||
            a_param_info->is_noalias != b_param_info->is_noalias)
        {
            return false;
        }
    }
    return true;
}

static uint32_t hash_const_val(ConstExprValue *const_val) {
    assert(const_val->special == ConstValSpecialStatic);
    switch (const_val->type->id) {
        case TypeTableEntryIdBool:
            return const_val->data.x_bool ? 127863866 : 215080464;
        case TypeTableEntryIdMetaType:
            return hash_ptr(const_val->data.x_type);
        case TypeTableEntryIdVoid:
            return 4149439618;
        case TypeTableEntryIdInt:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdEnumTag:
            return ((uint32_t)(bignum_to_twos_complement(&const_val->data.x_bignum) % UINT32_MAX)) * 1331471175;
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdNumLitFloat:
            return const_val->data.x_bignum.data.x_float * UINT32_MAX;
        case TypeTableEntryIdPointer:
            return hash_ptr(const_val->data.x_ptr.base_ptr) + hash_size(const_val->data.x_ptr.index);
        case TypeTableEntryIdUndefLit:
            return 162837799;
        case TypeTableEntryIdNullLit:
            return 844854567;
        case TypeTableEntryIdArray:
            // TODO better hashing algorithm
            return 1166190605;
        case TypeTableEntryIdStruct:
            // TODO better hashing algorithm
            return 1532530855;
        case TypeTableEntryIdUnion:
            // TODO better hashing algorithm
            return 2709806591;
        case TypeTableEntryIdMaybe:
            if (const_val->data.x_maybe) {
                return hash_const_val(const_val->data.x_maybe) * 1992916303;
            } else {
                return 4016830364;
            }
        case TypeTableEntryIdErrorUnion:
            // TODO better hashing algorithm
            return 3415065496;
        case TypeTableEntryIdPureError:
            // TODO better hashing algorithm
            return 2630160122;
        case TypeTableEntryIdEnum:
            // TODO better hashing algorithm
            return 31643936;
        case TypeTableEntryIdFn:
            return hash_ptr(const_val->data.x_fn);
        case TypeTableEntryIdTypeDecl:
            return hash_ptr(const_val->data.x_type);
        case TypeTableEntryIdNamespace:
            return hash_ptr(const_val->data.x_import);
        case TypeTableEntryIdBlock:
            return hash_ptr(const_val->data.x_block);
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
            zig_unreachable();
    }
    zig_unreachable();
}

uint32_t generic_fn_type_id_hash(GenericFnTypeId *id) {
    uint32_t result = 0;
    result += hash_ptr(id->fn_entry);
    for (size_t i = 0; i < id->param_count; i += 1) {
        ConstExprValue *generic_param = &id->params[i];
        if (generic_param->special != ConstValSpecialRuntime) {
            result += hash_const_val(generic_param);
            result += hash_ptr(generic_param->type);
        }
    }
    return result;
}

bool generic_fn_type_id_eql(GenericFnTypeId *a, GenericFnTypeId *b) {
    assert(a->fn_entry);
    if (a->fn_entry != b->fn_entry) return false;
    assert(a->param_count == b->param_count);
    for (size_t i = 0; i < a->param_count; i += 1) {
        ConstExprValue *a_val = &a->params[i];
        ConstExprValue *b_val = &b->params[i];
        if (a_val->type != b_val->type) return false;
        if (a_val->special != ConstValSpecialRuntime && b_val->special != ConstValSpecialRuntime) {
            assert(a_val->special == ConstValSpecialStatic);
            assert(b_val->special == ConstValSpecialStatic);
            if (!const_values_equal(a_val, b_val)) {
                return false;
            }
        } else {
            assert(a_val->special == ConstValSpecialRuntime && b_val->special == ConstValSpecialRuntime);
        }
    }
    return true;
}

uint32_t fn_eval_hash(Scope* scope) {
    uint32_t result = 0;
    while (scope) {
        if (scope->id == ScopeIdVarDecl) {
            ScopeVarDecl *var_scope = (ScopeVarDecl *)scope;
            result += hash_const_val(&var_scope->var->value);
        } else if (scope->id == ScopeIdFnDef) {
            ScopeFnDef *fn_scope = (ScopeFnDef *)scope;
            result += hash_ptr(fn_scope->fn_entry);
            return result;
        } else {
            zig_unreachable();
        }

        scope = scope->parent;
    }
    zig_unreachable();
}

bool fn_eval_eql(Scope *a, Scope *b) {
    while (a && b) {
        if (a->id != b->id)
            return false;

        if (a->id == ScopeIdVarDecl) {
            ScopeVarDecl *a_var_scope = (ScopeVarDecl *)a;
            ScopeVarDecl *b_var_scope = (ScopeVarDecl *)b;
            if (a_var_scope->var->value.type != b_var_scope->var->value.type)
                return false;
            if (!const_values_equal(&a_var_scope->var->value, &b_var_scope->var->value))
                return false;
        } else if (a->id == ScopeIdFnDef) {
            ScopeFnDef *a_fn_scope = (ScopeFnDef *)a;
            ScopeFnDef *b_fn_scope = (ScopeFnDef *)b;
            if (a_fn_scope->fn_entry != b_fn_scope->fn_entry)
                return false;

            return true;
        } else {
            zig_unreachable();
        }

        a = a->parent;
        b = b->parent;
    }
    return false;
}

bool type_has_bits(TypeTableEntry *type_entry) {
    assert(type_entry);
    assert(type_entry->id != TypeTableEntryIdInvalid);
    assert(type_has_zero_bits_known(type_entry));
    return !type_entry->zero_bits;
}

static TypeTableEntry *first_struct_field_type(TypeTableEntry *type_entry) {
    assert(type_entry->id == TypeTableEntryIdStruct);
    for (uint32_t i = 0; i < type_entry->data.structure.src_field_count; i += 1) {
        TypeStructField *tsf = &type_entry->data.structure.fields[i];
        if (tsf->gen_index == 0) {
            return tsf->type_entry;
        }
    }
    zig_unreachable();
}

static TypeTableEntry *type_of_first_thing_in_memory(TypeTableEntry *type_entry) {
    assert(type_has_bits(type_entry));
    switch (type_entry->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdVoid:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdVar:
            zig_unreachable();
        case TypeTableEntryIdArray:
            return type_of_first_thing_in_memory(type_entry->data.array.child_type);
        case TypeTableEntryIdStruct:
            return type_of_first_thing_in_memory(first_struct_field_type(type_entry));
        case TypeTableEntryIdUnion:
            zig_panic("TODO");
        case TypeTableEntryIdMaybe:
            return type_of_first_thing_in_memory(type_entry->data.maybe.child_type);
        case TypeTableEntryIdErrorUnion:
            return type_of_first_thing_in_memory(type_entry->data.error.child_type);
        case TypeTableEntryIdTypeDecl:
            return type_of_first_thing_in_memory(type_entry->data.type_decl.canonical_type);
        case TypeTableEntryIdEnum:
            return type_entry->data.enumeration.tag_type;
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdEnumTag:
            return type_entry;
    }
    zig_unreachable();
}

bool type_requires_comptime(TypeTableEntry *type_entry) {
    switch (get_underlying_type(type_entry)->id) {
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdNullLit:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdNamespace:
        case TypeTableEntryIdBlock:
        case TypeTableEntryIdBoundFn:
            return true;
        case TypeTableEntryIdArray:
        case TypeTableEntryIdStruct:
        case TypeTableEntryIdUnion:
        case TypeTableEntryIdMaybe:
        case TypeTableEntryIdErrorUnion:
        case TypeTableEntryIdEnum:
        case TypeTableEntryIdPureError:
        case TypeTableEntryIdFn:
        case TypeTableEntryIdBool:
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdPointer:
        case TypeTableEntryIdEnumTag:
        case TypeTableEntryIdVoid:
            return false;
    }
    zig_unreachable();
}

uint64_t get_memcpy_align(CodeGen *g, TypeTableEntry *type_entry) {
    TypeTableEntry *first_type_in_mem = type_of_first_thing_in_memory(type_entry);
    return LLVMABISizeOfType(g->target_data_ref, first_type_in_mem->type_ref);
}

void init_const_str_lit(CodeGen *g, ConstExprValue *const_val, Buf *str) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = get_array_type(g, g->builtin_types.entry_u8, buf_len(str));
    const_val->data.x_array.elements = allocate<ConstExprValue>(buf_len(str));
    const_val->data.x_array.size = buf_len(str);

    for (size_t i = 0; i < buf_len(str); i += 1) {
        ConstExprValue *this_char = &const_val->data.x_array.elements[i];
        this_char->special = ConstValSpecialStatic;
        this_char->type = g->builtin_types.entry_u8;
        bignum_init_unsigned(&this_char->data.x_bignum, buf_ptr(str)[i]);
    }
}

ConstExprValue *create_const_str_lit(CodeGen *g, Buf *str) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_str_lit(g, const_val, str);
    return const_val;
}

void init_const_c_str_lit(CodeGen *g, ConstExprValue *const_val, Buf *str) {
    // first we build the underlying array
    size_t len_with_null = buf_len(str) + 1;
    ConstExprValue *array_val = allocate<ConstExprValue>(1);
    array_val->special = ConstValSpecialStatic;
    array_val->type = get_array_type(g, g->builtin_types.entry_u8, len_with_null);
    array_val->data.x_array.elements = allocate<ConstExprValue>(len_with_null);
    array_val->data.x_array.size = len_with_null;
    for (size_t i = 0; i < buf_len(str); i += 1) {
        ConstExprValue *this_char = &array_val->data.x_array.elements[i];
        this_char->special = ConstValSpecialStatic;
        this_char->type = g->builtin_types.entry_u8;
        bignum_init_unsigned(&this_char->data.x_bignum, buf_ptr(str)[i]);
    }
    ConstExprValue *null_char = &array_val->data.x_array.elements[len_with_null - 1];
    null_char->special = ConstValSpecialStatic;
    null_char->type = g->builtin_types.entry_u8;
    bignum_init_unsigned(&null_char->data.x_bignum, 0);

    // then make the pointer point to it
    const_val->special = ConstValSpecialStatic;
    const_val->type = get_pointer_to_type(g, g->builtin_types.entry_u8, true);
    const_val->data.x_ptr.base_ptr = array_val;
    const_val->data.x_ptr.index = 0;
    const_val->data.x_ptr.special = ConstPtrSpecialCStr;
}
ConstExprValue *create_const_c_str_lit(CodeGen *g, Buf *str) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_c_str_lit(g, const_val, str);
    return const_val;
}

void init_const_unsigned_negative(ConstExprValue *const_val, TypeTableEntry *type, uint64_t x, bool negative) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bignum_init_unsigned(&const_val->data.x_bignum, x);
    const_val->data.x_bignum.is_negative = negative;
}

ConstExprValue *create_const_unsigned_negative(TypeTableEntry *type, uint64_t x, bool negative) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_unsigned_negative(const_val, type, x, negative);
    return const_val;
}

void init_const_usize(CodeGen *g, ConstExprValue *const_val, uint64_t x) {
    return init_const_unsigned_negative(const_val, g->builtin_types.entry_usize, x, false);
}

ConstExprValue *create_const_usize(CodeGen *g, uint64_t x) {
    return create_const_unsigned_negative(g->builtin_types.entry_usize, x, false);
}

void init_const_signed(ConstExprValue *const_val, TypeTableEntry *type, int64_t x) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bignum_init_signed(&const_val->data.x_bignum, x);
}

ConstExprValue *create_const_signed(TypeTableEntry *type, int64_t x) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_signed(const_val, type, x);
    return const_val;
}

void init_const_float(ConstExprValue *const_val, TypeTableEntry *type, double value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    bignum_init_float(&const_val->data.x_bignum, value);
}

ConstExprValue *create_const_float(TypeTableEntry *type, double value) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_float(const_val, type, value);
    return const_val;
}

void init_const_enum_tag(ConstExprValue *const_val, TypeTableEntry *type, uint64_t tag) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = type;
    const_val->data.x_enum.tag = tag;
}

ConstExprValue *create_const_enum_tag(TypeTableEntry *type, uint64_t tag) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_enum_tag(const_val, type, tag);
    return const_val;
}

void init_const_bool(CodeGen *g, ConstExprValue *const_val, bool value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = g->builtin_types.entry_bool;
    const_val->data.x_bool = value;
}

ConstExprValue *create_const_bool(CodeGen *g, bool value) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_bool(g, const_val, value);
    return const_val;
}

void init_const_runtime(ConstExprValue *const_val, TypeTableEntry *type) {
    const_val->special = ConstValSpecialRuntime;
    const_val->type = type;
}

ConstExprValue *create_const_runtime(TypeTableEntry *type) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_runtime(const_val, type);
    return const_val;
}

void init_const_type(CodeGen *g, ConstExprValue *const_val, TypeTableEntry *type_value) {
    const_val->special = ConstValSpecialStatic;
    const_val->type = g->builtin_types.entry_type;
    const_val->data.x_type = type_value;
}

ConstExprValue *create_const_type(CodeGen *g, TypeTableEntry *type_value) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_type(g, const_val, type_value);
    return const_val;
}

void init_const_slice(CodeGen *g, ConstExprValue *const_val, ConstExprValue *array_val,
        size_t start, size_t len, bool is_const)
{
    assert(array_val->type->id == TypeTableEntryIdArray);

    const_val->special = ConstValSpecialStatic;
    const_val->type = get_slice_type(g, array_val->type->data.array.child_type, is_const);
    const_val->data.x_struct.fields = allocate<ConstExprValue>(2);

    init_const_ptr(g, &const_val->data.x_struct.fields[slice_ptr_index], array_val, start, is_const);
    init_const_usize(g, &const_val->data.x_struct.fields[slice_len_index], len);
}

ConstExprValue *create_const_slice(CodeGen *g, ConstExprValue *array_val, size_t start, size_t len, bool is_const) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_slice(g, const_val, array_val, start, len, is_const);
    return const_val;
}

void init_const_ptr(CodeGen *g, ConstExprValue *const_val, ConstExprValue *base_ptr, size_t index, bool is_const) {
    TypeTableEntry *child_type;
    if (index == SIZE_MAX) {
        child_type = base_ptr->type;
    } else {
        assert(base_ptr->type->id == TypeTableEntryIdArray);
        child_type = base_ptr->type->data.array.child_type;
    }

    const_val->special = ConstValSpecialStatic;
    const_val->type = get_pointer_to_type(g, child_type, is_const);
    const_val->data.x_ptr.base_ptr = base_ptr;
    const_val->data.x_ptr.index = index;
}

ConstExprValue *create_const_ptr(CodeGen *g, ConstExprValue *base_ptr, size_t index, bool is_const) {
    ConstExprValue *const_val = allocate<ConstExprValue>(1);
    init_const_ptr(g, const_val, base_ptr, index, is_const);
    return const_val;
}

void ensure_complete_type(CodeGen *g, TypeTableEntry *type_entry) {
    if (type_entry->id == TypeTableEntryIdStruct) {
        if (!type_entry->data.structure.complete)
            resolve_struct_type(g, type_entry);
    } else if (type_entry->id == TypeTableEntryIdEnum) {
        if (!type_entry->data.enumeration.complete)
            resolve_enum_type(g, type_entry);
    } else if (type_entry->id == TypeTableEntryIdUnion) {
        if (!type_entry->data.unionation.complete)
            resolve_union_type(g, type_entry);
    }
}

void type_ensure_zero_bits_known(CodeGen *g, TypeTableEntry *type_entry) {
    if (type_entry->id == TypeTableEntryIdStruct) {
        resolve_struct_zero_bits(g, type_entry);
    } else if (type_entry->id == TypeTableEntryIdEnum) {
        resolve_enum_zero_bits(g, type_entry);
    } else if (type_entry->id == TypeTableEntryIdUnion) {
        resolve_union_zero_bits(g, type_entry);
    }
}

bool ir_get_var_is_comptime(VariableTableEntry *var) {
    if (!var->is_comptime)
        return false;
    if (var->is_comptime->other)
        return var->is_comptime->other->value.data.x_bool;
    return var->is_comptime->value.data.x_bool;
}

bool const_values_equal(ConstExprValue *a, ConstExprValue *b) {
    assert(a->type == b->type);
    assert(a->special == ConstValSpecialStatic);
    assert(b->special == ConstValSpecialStatic);
    switch (a->type->id) {
        case TypeTableEntryIdEnum:
            {
                ConstEnumValue *enum1 = &a->data.x_enum;
                ConstEnumValue *enum2 = &b->data.x_enum;
                if (enum1->tag == enum2->tag) {
                    TypeEnumField *enum_field = &a->type->data.enumeration.fields[enum1->tag];
                    if (type_has_bits(enum_field->type_entry)) {
                        zig_panic("TODO const expr analyze enum special value for equality");
                    } else {
                        return true;
                    }
                }
                return false;
            }
        case TypeTableEntryIdMetaType:
            return a->data.x_type == b->data.x_type;
        case TypeTableEntryIdVoid:
            return true;
        case TypeTableEntryIdPureError:
            return a->data.x_pure_err == b->data.x_pure_err;
        case TypeTableEntryIdFn:
            return a->data.x_fn == b->data.x_fn;
        case TypeTableEntryIdBool:
            return a->data.x_bool == b->data.x_bool;
        case TypeTableEntryIdInt:
        case TypeTableEntryIdFloat:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdEnumTag:
            return bignum_cmp_eq(&a->data.x_bignum, &b->data.x_bignum);
        case TypeTableEntryIdPointer:
            if (a->data.x_ptr.index != b->data.x_ptr.index)
                return false;
            return a->data.x_ptr.base_ptr == b->data.x_ptr.base_ptr;
        case TypeTableEntryIdArray:
            zig_panic("TODO");
        case TypeTableEntryIdStruct:
            zig_panic("TODO");
        case TypeTableEntryIdUnion:
            zig_panic("TODO");
        case TypeTableEntryIdUndefLit:
            zig_panic("TODO");
        case TypeTableEntryIdNullLit:
            zig_panic("TODO");
        case TypeTableEntryIdMaybe:
            zig_panic("TODO");
        case TypeTableEntryIdErrorUnion:
            zig_panic("TODO");
        case TypeTableEntryIdTypeDecl:
            zig_panic("TODO");
        case TypeTableEntryIdNamespace:
            return a->data.x_import == b->data.x_import;
        case TypeTableEntryIdBlock:
            return a->data.x_block == b->data.x_block;
        case TypeTableEntryIdBoundFn:
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdVar:
            zig_unreachable();
    }
    zig_unreachable();
}

static bool int_type_depends_on_compile_var(CodeGen *g, TypeTableEntry *int_type) {
    assert(int_type->id == TypeTableEntryIdInt);

    for (size_t i = 0; i < CIntTypeCount; i += 1) {
        if (int_type == g->builtin_types.entry_c_int[i]) {
            return true;
        }
    }
    return false;
}

static uint64_t max_unsigned_val(TypeTableEntry *type_entry) {
    assert(type_entry->id == TypeTableEntryIdInt);
    if (type_entry->data.integral.bit_count == 64) {
        return UINT64_MAX;
    } else if (type_entry->data.integral.bit_count == 32) {
        return UINT32_MAX;
    } else if (type_entry->data.integral.bit_count == 16) {
        return UINT16_MAX;
    } else if (type_entry->data.integral.bit_count == 8) {
        return UINT8_MAX;
    } else {
        zig_unreachable();
    }
}

static int64_t max_signed_val(TypeTableEntry *type_entry) {
    assert(type_entry->id == TypeTableEntryIdInt);
    if (type_entry->data.integral.bit_count == 64) {
        return INT64_MAX;
    } else if (type_entry->data.integral.bit_count == 32) {
        return INT32_MAX;
    } else if (type_entry->data.integral.bit_count == 16) {
        return INT16_MAX;
    } else if (type_entry->data.integral.bit_count == 8) {
        return INT8_MAX;
    } else {
        zig_unreachable();
    }
}

static int64_t min_signed_val(TypeTableEntry *type_entry) {
    assert(type_entry->id == TypeTableEntryIdInt);
    if (type_entry->data.integral.bit_count == 64) {
        return INT64_MIN;
    } else if (type_entry->data.integral.bit_count == 32) {
        return INT32_MIN;
    } else if (type_entry->data.integral.bit_count == 16) {
        return INT16_MIN;
    } else if (type_entry->data.integral.bit_count == 8) {
        return INT8_MIN;
    } else {
        zig_unreachable();
    }
}

void eval_min_max_value(CodeGen *g, TypeTableEntry *type_entry, ConstExprValue *const_val, bool is_max) {
    if (type_entry->id == TypeTableEntryIdInt) {
        const_val->special = ConstValSpecialStatic;
        const_val->depends_on_compile_var = int_type_depends_on_compile_var(g, type_entry);
        if (is_max) {
            if (type_entry->data.integral.is_signed) {
                int64_t val = max_signed_val(type_entry);
                bignum_init_signed(&const_val->data.x_bignum, val);
            } else {
                uint64_t val = max_unsigned_val(type_entry);
                bignum_init_unsigned(&const_val->data.x_bignum, val);
            }
        } else {
            if (type_entry->data.integral.is_signed) {
                int64_t val = min_signed_val(type_entry);
                bignum_init_signed(&const_val->data.x_bignum, val);
            } else {
                bignum_init_unsigned(&const_val->data.x_bignum, 0);
            }
        }
    } else if (type_entry->id == TypeTableEntryIdFloat) {
        zig_panic("TODO analyze_min_max_value float");
    } else if (type_entry->id == TypeTableEntryIdBool) {
        const_val->special = ConstValSpecialStatic;
        const_val->data.x_bool = is_max;
    } else if (type_entry->id == TypeTableEntryIdVoid) {
        // nothing to do
    } else {
        zig_unreachable();
    }
}

void render_const_value(Buf *buf, ConstExprValue *const_val) {
    switch (const_val->special) {
        case ConstValSpecialRuntime:
            zig_unreachable();
        case ConstValSpecialUndef:
            buf_appendf(buf, "undefined");
            return;
        case ConstValSpecialZeroes:
            buf_appendf(buf, "zeroes");
            return;
        case ConstValSpecialStatic:
            break;
    }
    assert(const_val->type);

    TypeTableEntry *canon_type = get_underlying_type(const_val->type);
    switch (canon_type->id) {
        case TypeTableEntryIdTypeDecl:
            zig_unreachable();
        case TypeTableEntryIdInvalid:
            buf_appendf(buf, "(invalid)");
            return;
        case TypeTableEntryIdVar:
            buf_appendf(buf, "(var)");
            return;
        case TypeTableEntryIdVoid:
            buf_appendf(buf, "{}");
            return;
        case TypeTableEntryIdNumLitFloat:
            buf_appendf(buf, "%f", const_val->data.x_bignum.data.x_float);
            return;
        case TypeTableEntryIdNumLitInt:
            {
                BigNum *bignum = &const_val->data.x_bignum;
                const char *negative_str = bignum->is_negative ? "-" : "";
                buf_appendf(buf, "%s%llu", negative_str, bignum->data.x_uint);
                return;
            }
        case TypeTableEntryIdMetaType:
            buf_appendf(buf, "%s", buf_ptr(&const_val->data.x_type->name));
            return;
        case TypeTableEntryIdInt:
            {
                BigNum *bignum = &const_val->data.x_bignum;
                assert(bignum->kind == BigNumKindInt);
                const char *negative_str = bignum->is_negative ? "-" : "";
                buf_appendf(buf, "%s%llu", negative_str, bignum->data.x_uint);
            }
            return;
        case TypeTableEntryIdFloat:
            {
                BigNum *bignum = &const_val->data.x_bignum;
                assert(bignum->kind == BigNumKindFloat);
                buf_appendf(buf, "%f", bignum->data.x_float);
            }
            return;
        case TypeTableEntryIdUnreachable:
            buf_appendf(buf, "@unreachable()");
            return;
        case TypeTableEntryIdBool:
            {
                const char *value = const_val->data.x_bool ? "true" : "false";
                buf_appendf(buf, "%s", value);
                return;
            }
        case TypeTableEntryIdPointer:
            buf_appendf(buf, "&");
            if (const_val->data.x_ptr.special == ConstPtrSpecialRuntime) {
                buf_appendf(buf, "(runtime pointer value)");
            } else if (const_val->data.x_ptr.special == ConstPtrSpecialCStr) {
                buf_appendf(buf, "(c str lit)");
            } else {
                render_const_value(buf, const_ptr_pointee(const_val));
            }
            return;
        case TypeTableEntryIdFn:
            {
                FnTableEntry *fn_entry = const_val->data.x_fn;
                buf_appendf(buf, "%s", buf_ptr(&fn_entry->symbol_name));
                return;
            }
        case TypeTableEntryIdBlock:
            {
                AstNode *node = const_val->data.x_block->source_node;
                buf_appendf(buf, "(scope:%zu:%zu)", node->line + 1, node->column + 1);
                return;
            }
        case TypeTableEntryIdArray:
            {
                uint64_t len = canon_type->data.array.len;
                buf_appendf(buf, "%s{", buf_ptr(&canon_type->name));
                for (uint64_t i = 0; i < len; i += 1) {
                    if (i != 0)
                        buf_appendf(buf, ",");
                    ConstExprValue *child_value = &const_val->data.x_array.elements[i];
                    render_const_value(buf, child_value);
                }
                buf_appendf(buf, "}");
                return;
            }
        case TypeTableEntryIdNullLit:
            {
                buf_appendf(buf, "null");
                return;
            }
        case TypeTableEntryIdUndefLit:
            {
                buf_appendf(buf, "undefined");
                return;
            }
        case TypeTableEntryIdMaybe:
            {
                if (const_val->data.x_maybe) {
                    render_const_value(buf, const_val->data.x_maybe);
                } else {
                    buf_appendf(buf, "null");
                }
                return;
            }
        case TypeTableEntryIdNamespace:
            {
                ImportTableEntry *import = const_val->data.x_import;
                if (import->c_import_node) {
                    buf_appendf(buf, "(namespace from C import)");
                } else {
                    buf_appendf(buf, "(namespace: %s)", buf_ptr(import->path));
                }
                return;
            }
        case TypeTableEntryIdBoundFn:
            {
                FnTableEntry *fn_entry = const_val->data.x_bound_fn.fn;
                buf_appendf(buf, "(bound fn %s)", buf_ptr(&fn_entry->symbol_name));
                return;
            }
        case TypeTableEntryIdStruct:
            {
                buf_appendf(buf, "(struct %s constant)", buf_ptr(&canon_type->name));
                return;
            }
        case TypeTableEntryIdEnum:
            {
                buf_appendf(buf, "(enum %s constant)", buf_ptr(&canon_type->name));
                return;
            }
        case TypeTableEntryIdErrorUnion:
            {
                buf_appendf(buf, "(error union %s constant)", buf_ptr(&canon_type->name));
                return;
            }
        case TypeTableEntryIdUnion:
            {
                buf_appendf(buf, "(union %s constant)", buf_ptr(&canon_type->name));
                return;
            }
        case TypeTableEntryIdPureError:
            {
                buf_appendf(buf, "(pure error constant)");
                return;
            }
        case TypeTableEntryIdEnumTag:
            {
                TypeTableEntry *enum_type = canon_type->data.enum_tag.enum_type;
                TypeEnumField *field = &enum_type->data.enumeration.fields[const_val->data.x_bignum.data.x_uint];
                buf_appendf(buf, "%s.%s", buf_ptr(&enum_type->name), buf_ptr(field->name));
                return;
            }
    }
    zig_unreachable();
}

