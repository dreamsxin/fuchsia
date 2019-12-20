// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/c_generator.h"

#include "fidl/attributes.h"
#include "fidl/flat_ast.h"
#include "fidl/names.h"
#include "fidl/type_shape.h"

namespace fidl {

namespace {

// RAII helper class to reset the iostream to its original flags.
class IOFlagsGuard {
 public:
  explicit IOFlagsGuard(std::ostream* stream) : stream_(stream), flags_(stream_->flags()) {}

  ~IOFlagsGuard() { stream_->setf(flags_); }

 private:
  std::ostream* stream_;
  std::ios::fmtflags flags_;
};

// Various string values are looked up or computed in these
// functions. Nothing else should be dealing in string literals, or
// computing strings from these or AST values.

constexpr const char* kIndent = "    ";

const std::set<std::string> allowed_c_unions{{
    "fuchsia_cobalt_EventPayload",
    "fuchsia_cobalt_Value",
    "fuchsia_device_Controller_Bind_Result",
    "fuchsia_device_Controller_GetDevicePowerCaps_Result",
    "fuchsia_device_Controller_GetPowerStateMapping_Result",
    "fuchsia_device_Controller_GetTopologicalPath_Result",
    "fuchsia_device_Controller_Rebind_Result",
    "fuchsia_device_Controller_Resume_Result",
    "fuchsia_device_Controller_ScheduleUnbind_Result",
    "fuchsia_device_Controller_UpdatePowerStateMapping_Result",
    "fuchsia_device_manager_Coordinator_AddCompositeDevice_Result",
    "fuchsia_device_manager_Coordinator_AddDevice_Result",
    "fuchsia_device_manager_Coordinator_AddDeviceInvisible_Result",
    "fuchsia_device_manager_Coordinator_AddMetadata_Result",
    "fuchsia_device_manager_Coordinator_BindDevice_Result",
    "fuchsia_device_manager_Coordinator_DirectoryWatch_Result",
    "fuchsia_device_manager_Coordinator_GetMetadata_Result",
    "fuchsia_device_manager_Coordinator_GetMetadataSize_Result",
    "fuchsia_device_manager_Coordinator_GetTopologicalPath_Result",
    "fuchsia_device_manager_Coordinator_LoadFirmware_Result",
    "fuchsia_device_manager_Coordinator_MakeVisible_Result",
    "fuchsia_device_manager_Coordinator_PublishMetadata_Result",
    "fuchsia_device_manager_Coordinator_RunCompatibilityTests_Result",
    "fuchsia_device_manager_DeviceController_CompleteRemoval_Result",
    "fuchsia_device_manager_DeviceController_Unbind_Result",
    "fuchsia_device_NameProvider_GetDeviceName_Result",
    "fuchsia_hardware_display_Controller_ImportImageForCapture_Result",
    "fuchsia_hardware_display_Controller_IsCaptureSupported_Result",
    "fuchsia_hardware_display_Controller_ReleaseCapture_Result",
    "fuchsia_hardware_display_Controller_StartCapture_Result",
    "fuchsia_hardware_power_statecontrol_Admin_Suspend_Result",
    "fuchsia_hardware_serial_NewDevice_Read_Result",
    "fuchsia_hardware_serial_NewDevice_Write_Result",
    "fuchsia_hardware_usb_peripheral_Device_SetConfiguration_Result",
    "fuchsia_io_NodeInfo",
    "fuchsia_paver_BootManager_QueryActiveConfiguration_Result",
    "fuchsia_paver_BootManager_QueryConfigurationStatus_Result",
    "fuchsia_paver_DataSink_ReadAsset_Result",
    "fuchsia_paver_DataSink_WipeVolume_Result",
    "fuchsia_paver_ReadResult",
    "fuchsia_sysmem_SecureMem_GetPhysicalSecureHeaps_Result",
    "fuchsia_sysmem_SecureMem_SetPhysicalSecureHeaps_Result",
    "fuchsia_wlan_stats_MlmeStats",
}};

const std::vector<std::string> allowed_c_union_prefixes{
  "example_", "fidl_test_example_codingtables_",
};

bool CUnionAllowed(const std::string& union_name) {
  if (allowed_c_unions.find(union_name) != allowed_c_unions.end()) {
    return true;
  }
  for (const auto& prefix : allowed_c_union_prefixes) {
    if (union_name.find(prefix) == 0) {
      return true;
    }
  }
  return false;
}

CGenerator::Member MessageHeader() {
  return {
      flat::Type::Kind::kIdentifier,
      flat::Decl::Kind::kStruct,
      "fidl_message_header_t",
      "hdr",
      {},
      {},
      types::Nullability::kNonnullable,
      {},
  };
}

CGenerator::Member EmptyStructMember() {
  return {
      .kind = flat::Type::Kind::kPrimitive,
      .type = NamePrimitiveCType(types::PrimitiveSubtype::kUint8),

      // Prepend the reserved uint8_t field with a single underscore, which is
      // for reserved identifiers (see ISO C standard, section 7.1.3
      // <http://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf>).
      .name = "_reserved",
  };
}

CGenerator::Transport ParseTransport(std::string_view view) {
  return CGenerator::Transport::Channel;
}

bool ShouldRecursiveDeprecate(const std::vector<CGenerator::Member>& members) {
#ifdef FIDLC_DEPRECATE_C_UNIONS
  for (const auto& m : members) {
    switch (m.decl_kind) {
      case flat::Decl::Kind::kUnion:
        return true;
      default:
        break;
    }
  }
#endif
  return false;
}

// Functions named "Emit..." are called to actually emit to an std::ostream
// is here. No other functions should directly emit to the streams.

void EmitFileComment(std::ostream* file) {
  *file << "// WARNING: This file is machine generated by fidlc.\n\n";
}

void EmitHeaderGuard(std::ostream* file) {
  // TODO(704) Generate an appropriate header guard name.
  *file << "#pragma once\n";
}

void EmitIncludeHeader(std::ostream* file, std::string_view header) {
  *file << "#include " << header << "\n";
}

void EmitBeginExternC(std::ostream* file) {
  *file << "#if defined(__cplusplus)\nextern \"C\" {\n#endif\n";
}

void EmitEndExternC(std::ostream* file) { *file << "#if defined(__cplusplus)\n}\n#endif\n"; }

void EmitBlank(std::ostream* file) { *file << "\n"; }

void EmitMemberDecl(std::ostream* file, const CGenerator::Member& member) {
  *file << member.type << " " << member.name;
  for (uint32_t array_count : member.array_counts) {
    *file << "[" << array_count << "]";
  }
}

void EmitMethodInParamDecl(std::ostream* file, const CGenerator::Member& member) {
  switch (member.kind) {
    case flat::Type::Kind::kArray:
      *file << "const " << member.type << " " << member.name;
      for (uint32_t array_count : member.array_counts) {
        *file << "[" << array_count << "]";
      }
      break;
    case flat::Type::Kind::kVector:
      *file << "const " << member.element_type << "* " << member.name << "_data, "
            << "size_t " << member.name << "_count";
      break;
    case flat::Type::Kind::kString:
      *file << "const char* " << member.name << "_data, "
            << "size_t " << member.name << "_size";
      break;
    case flat::Type::Kind::kHandle:
    case flat::Type::Kind::kRequestHandle:
    case flat::Type::Kind::kPrimitive:
      *file << member.type << " " << member.name;
      break;
    case flat::Type::Kind::kIdentifier:
      switch (member.decl_kind) {
        case flat::Decl::Kind::kConst:
        case flat::Decl::Kind::kService:
        case flat::Decl::Kind::kTypeAlias:
          assert(false && "bad decl kind for member");
          break;
        case flat::Decl::Kind::kBits:
        case flat::Decl::Kind::kEnum:
        case flat::Decl::Kind::kProtocol:
          *file << member.type << " " << member.name;
          break;
        case flat::Decl::Kind::kStruct:
        case flat::Decl::Kind::kTable:
        case flat::Decl::Kind::kUnion:
        case flat::Decl::Kind::kXUnion:
          switch (member.nullability) {
            case types::Nullability::kNullable:
              *file << "const " << member.type << " " << member.name;
              break;
            case types::Nullability::kNonnullable:
              *file << "const " << member.type << "* " << member.name;
              break;
          }
          break;
      }
      break;
  }
}

void EmitMethodOutParamDecl(std::ostream* file, const CGenerator::Member& member) {
  switch (member.kind) {
    case flat::Type::Kind::kArray:
      *file << member.type << " out_" << member.name;
      for (uint32_t array_count : member.array_counts) {
        *file << "[" << array_count << "]";
      }
      break;
    case flat::Type::Kind::kVector:
      *file << member.element_type << "* " << member.name << "_buffer, "
            << "size_t " << member.name << "_capacity, "
            << "size_t* out_" << member.name << "_count";
      break;
    case flat::Type::Kind::kString:
      *file << "char* " << member.name << "_buffer, "
            << "size_t " << member.name << "_capacity, "
            << "size_t* out_" << member.name << "_size";
      break;
    case flat::Type::Kind::kHandle:
    case flat::Type::Kind::kRequestHandle:
    case flat::Type::Kind::kPrimitive:
      *file << member.type << "* out_" << member.name;
      break;
    case flat::Type::Kind::kIdentifier:
      switch (member.decl_kind) {
        case flat::Decl::Kind::kConst:
        case flat::Decl::Kind::kService:
        case flat::Decl::Kind::kTypeAlias:
          assert(false && "bad decl kind for member");
          break;
        case flat::Decl::Kind::kBits:
        case flat::Decl::Kind::kEnum:
        case flat::Decl::Kind::kProtocol:
          *file << member.type << "* out_" << member.name;
          break;
        case flat::Decl::Kind::kStruct:
        case flat::Decl::Kind::kTable:
        case flat::Decl::Kind::kUnion:
        case flat::Decl::Kind::kXUnion:
          switch (member.nullability) {
            case types::Nullability::kNullable:
              *file << member.type << " out_" << member.name;
              break;
            case types::Nullability::kNonnullable:
              *file << member.type << "* out_" << member.name;
              break;
          }
          break;
      }
      break;
  }
}

void EmitClientMethodDecl(std::ostream* file, std::string_view method_name,
                          const std::vector<CGenerator::Member>& request,
                          const std::vector<CGenerator::Member>& response) {
  bool should_recursive_deprecate =
      ShouldRecursiveDeprecate(request) || ShouldRecursiveDeprecate(response);
  *file << "zx_status_t " << (should_recursive_deprecate ? " __attribute__ ((deprecated)) " : "")
        << method_name << "(zx_handle_t _channel";
  for (const auto& member : request) {
    *file << ", ";
    EmitMethodInParamDecl(file, member);
  }
  for (auto member : response) {
    *file << ", ";
    EmitMethodOutParamDecl(file, member);
  }
  *file << ")";
}

void EmitServerMethodDecl(std::ostream* file, std::string_view method_name,
                          const std::vector<CGenerator::Member>& request, bool has_response) {
  bool should_recursive_deprecate = ShouldRecursiveDeprecate(request);
  *file << "zx_status_t " << (should_recursive_deprecate ? " __attribute__ ((deprecated)) " : "")
        << " (*" << method_name << ")(void* ctx";
  for (const auto& member : request) {
    *file << ", ";
    EmitMethodInParamDecl(file, member);
  }
  if (has_response) {
    *file << ", fidl_txn_t* txn";
  }
  *file << ")";
}

void EmitServerDispatchDecl(std::ostream* file, std::string_view protocol_name) {
  *file << "zx_status_t " << protocol_name
        << "_dispatch(void* ctx, fidl_txn_t* txn, fidl_msg_t* msg, const " << protocol_name
        << "_ops_t* ops)";
}

void EmitServerTryDispatchDecl(std::ostream* file, std::string_view protocol_name) {
  *file << "zx_status_t " << protocol_name
        << "_try_dispatch(void* ctx, fidl_txn_t* txn, fidl_msg_t* msg, const " << protocol_name
        << "_ops_t* ops)";
}

void EmitServerReplyDecl(std::ostream* file, std::string_view method_name,
                         const std::vector<CGenerator::Member>& response) {
  bool should_recursive_deprecate = ShouldRecursiveDeprecate(response);
  *file << "zx_status_t " << (should_recursive_deprecate ? " __attribute__ ((deprecated)) " : "")
        << method_name << "_reply(fidl_txn_t* _txn";
  for (const auto& member : response) {
    *file << ", ";
    EmitMethodInParamDecl(file, member);
  }
  *file << ")";
}

bool IsStoredOutOfLine(const CGenerator::Member& member) {
  if (member.kind == flat::Type::Kind::kVector || member.kind == flat::Type::Kind::kString)
    return true;
  if (member.kind == flat::Type::Kind::kIdentifier) {
    if (member.decl_kind == flat::Decl::Kind::kXUnion ||
        member.decl_kind == flat::Decl::Kind::kTable)
      return true;
    if (member.nullability == types::Nullability::kNullable)
      return member.decl_kind == flat::Decl::Kind::kStruct ||
             member.decl_kind == flat::Decl::Kind::kUnion;
  }
  return false;
}

void EmitMeasureInParams(std::ostream* file, const std::vector<CGenerator::Member>& params) {
  for (const auto& member : params) {
    if (member.kind == flat::Type::Kind::kVector)
      *file << " + FIDL_ALIGN(sizeof(*" << member.name << "_data) * " << member.name << "_count)";
    else if (member.kind == flat::Type::Kind::kString)
      *file << " + FIDL_ALIGN(" << member.name << "_size)";
    else if (IsStoredOutOfLine(member))
      *file << " + (" << member.name << " ? FIDL_ALIGN(sizeof(*" << member.name << ")) : 0u)";
  }
}

void EmitParameterSizeValidation(std::ostream* file,
                                 const std::vector<CGenerator::Member>& params) {
  for (const auto& member : params) {
    if (member.max_num_elements == std::numeric_limits<uint32_t>::max())
      continue;
    std::string param_name;
    if (member.kind == flat::Type::Kind::kVector) {
      param_name = member.name + "_count";
    } else if (member.kind == flat::Type::Kind::kString) {
      param_name = member.name + "_size";
    } else {
      assert(false && "only vector/string has size limit");
    }
    *file << kIndent << "if (" << param_name << " > " << member.max_num_elements << ") {\n";
    *file << kIndent << kIndent << "return ZX_ERR_INVALID_ARGS;\n";
    *file << kIndent << "}\n";
  }
}

void EmitMeasureOutParams(std::ostream* file, const std::vector<CGenerator::Member>& params) {
  for (const auto& member : params) {
    if (member.kind == flat::Type::Kind::kVector)
      *file << " + FIDL_ALIGN(sizeof(*" << member.name << "_buffer) * " << member.name
            << "_capacity)";
    else if (member.kind == flat::Type::Kind::kString)
      *file << " + FIDL_ALIGN(" << member.name << "_capacity)";
    else if (IsStoredOutOfLine(member))
      *file << " + (out_" << member.name << " ? FIDL_ALIGN(sizeof(*out_" << member.name
            << ")) : 0u)";
  }
}

void EmitArraySizeOf(std::ostream* file, const CGenerator::Member& member) {
  for (const auto c : member.array_counts) {
    *file << c;
    *file << " * ";
  }
  *file << "sizeof(" << member.element_type << ")";
}

void EmitMagicNumberCheck(std::ostream* file) {
  *file << kIndent << "status = fidl_validate_txn_header(hdr);\n";
  *file << kIndent << "if (status != ZX_OK) {\n";
  *file << kIndent << kIndent << "zx_handle_close_many(msg->handles, msg->num_handles);\n";
  *file << kIndent << kIndent << "return status;\n";
  *file << kIndent << "}\n";
}

// This function assumes the |params| are part of a [Layout="Simple"] protocol.
// In particular, simple protocols don't have nullable structs or nested
// vectors. The only secondary objects they contain are top-level vectors and
// strings.
size_t CountSecondaryObjects(const std::vector<CGenerator::Member>& params) {
  size_t count = 0u;
  for (const auto& member : params) {
    if (IsStoredOutOfLine(member))
      ++count;
  }
  return count;
}

void EmitTxnHeader(std::ostream* file, const std::string& msg_name,
                   const std::string& ordinal_name) {
  *file << kIndent << "fidl_init_txn_header(&" << msg_name << "->hdr, 0, " << ordinal_name
        << ");\n";
}

void EmitLinearizeMessage(std::ostream* file, std::string_view receiver, std::string_view bytes,
                          const std::vector<CGenerator::Member>& request) {
  if (CountSecondaryObjects(request) > 0)
    *file << kIndent << "uint32_t _next = sizeof(*" << receiver << ");\n";
  for (const auto& member : request) {
    const auto& name = member.name;
    switch (member.kind) {
      case flat::Type::Kind::kArray:
        *file << kIndent << "memcpy(" << receiver << "->" << name << ", " << name << ", ";
        EmitArraySizeOf(file, member);
        *file << ");\n";
        break;
      case flat::Type::Kind::kVector:
        *file << kIndent << receiver << "->" << name << ".data = &" << bytes << "[_next];\n";
        *file << kIndent << receiver << "->" << name << ".count = " << name << "_count;\n";
        *file << kIndent << "memcpy(" << receiver << "->" << name << ".data, " << name
              << "_data, sizeof(*" << name << "_data) * " << name << "_count);\n";
        *file << kIndent << "_next += FIDL_ALIGN(sizeof(*" << name << "_data) * " << name
              << "_count);\n";
        break;
      case flat::Type::Kind::kString:
        *file << kIndent << receiver << "->" << name << ".data = &" << bytes << "[_next];\n";
        *file << kIndent << receiver << "->" << name << ".size = " << name << "_size;\n";
        *file << kIndent << "_next += FIDL_ALIGN(" << name << "_size);\n";
        *file << kIndent << "if (" << name << "_data) {\n";
        *file << kIndent << kIndent << "memcpy(" << receiver << "->" << name << ".data, " << name
              << "_data, " << name << "_size);\n";
        *file << kIndent << "} else {\n";
        *file << kIndent << kIndent << "if (" << name << "_size != 0) {\n";
        *file << kIndent << kIndent << kIndent << "return ZX_ERR_INVALID_ARGS;\n";
        *file << kIndent << kIndent << "}\n";
        if (member.nullability == types::Nullability::kNullable) {
          *file << kIndent << kIndent << receiver << "->" << name << ".data = NULL;\n";
        }
        *file << kIndent << "}\n";
        break;
      case flat::Type::Kind::kHandle:
      case flat::Type::Kind::kRequestHandle:
      case flat::Type::Kind::kPrimitive:
        *file << kIndent << receiver << "->" << name << " = " << name << ";\n";
        break;
      case flat::Type::Kind::kIdentifier:
        switch (member.decl_kind) {
          case flat::Decl::Kind::kConst:
          case flat::Decl::Kind::kService:
          case flat::Decl::Kind::kTypeAlias:
            assert(false && "bad decl kind for member");
            break;
          case flat::Decl::Kind::kBits:
          case flat::Decl::Kind::kEnum:
          case flat::Decl::Kind::kProtocol:
            *file << kIndent << receiver << "->" << name << " = " << name << ";\n";
            break;
          case flat::Decl::Kind::kTable:
            assert(false && "c-codegen for tables not yet implemented");
            break;
          case flat::Decl::Kind::kXUnion:
            assert(false && "c-codegen for extensible unions not yet implemented");
            break;
          case flat::Decl::Kind::kStruct:
          case flat::Decl::Kind::kUnion:
            switch (member.nullability) {
              case types::Nullability::kNullable:
                *file << kIndent << "if (" << name << ") {\n";
                *file << kIndent << kIndent << receiver << "->" << name << " = (void*)&" << bytes
                      << "[_next];\n";
                *file << kIndent << kIndent << "memcpy(" << receiver << "->" << name << ", " << name
                      << ", sizeof(*" << name << "));\n";
                *file << kIndent << kIndent << "_next += sizeof(*" << name << ");\n";
                *file << kIndent << "} else {\n";
                *file << kIndent << kIndent << receiver << "->" << name << " = NULL;\n";
                *file << kIndent << "}\n";
                break;
              case types::Nullability::kNonnullable:
                *file << kIndent << receiver << "->" << name << " = *" << name << ";\n";
                break;
            }
            break;
        }
    }
  }
}

// Various computational helper routines.

void BitsValue(const flat::Constant* constant, std::string* out_value) {
  std::ostringstream member_value;

  const flat::ConstantValue& const_val = constant->Value();
  switch (const_val.kind) {
    case flat::ConstantValue::Kind::kUint8: {
      auto value = static_cast<const flat::NumericConstantValue<uint8_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint16: {
      auto value = static_cast<const flat::NumericConstantValue<uint16_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint32: {
      auto value = static_cast<const flat::NumericConstantValue<uint32_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint64: {
      auto value = static_cast<const flat::NumericConstantValue<uint64_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kInt8:
    case flat::ConstantValue::Kind::kInt16:
    case flat::ConstantValue::Kind::kInt32:
    case flat::ConstantValue::Kind::kInt64:
    case flat::ConstantValue::Kind::kBool:
    case flat::ConstantValue::Kind::kFloat32:
    case flat::ConstantValue::Kind::kFloat64:
    case flat::ConstantValue::Kind::kString:
      assert(false && "bad primitive type for a bits declaration");
      break;
  }

  *out_value = member_value.str();
}

void EnumValue(const flat::Constant* constant, std::string* out_value) {
  std::ostringstream member_value;

  const flat::ConstantValue& const_val = constant->Value();
  switch (const_val.kind) {
    case flat::ConstantValue::Kind::kInt8: {
      auto value = static_cast<const flat::NumericConstantValue<int8_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kInt16: {
      auto value = static_cast<const flat::NumericConstantValue<int16_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kInt32: {
      auto value = static_cast<const flat::NumericConstantValue<int32_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kInt64: {
      auto value = static_cast<const flat::NumericConstantValue<int64_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint8: {
      auto value = static_cast<const flat::NumericConstantValue<uint8_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint16: {
      auto value = static_cast<const flat::NumericConstantValue<uint16_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint32: {
      auto value = static_cast<const flat::NumericConstantValue<uint32_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kUint64: {
      auto value = static_cast<const flat::NumericConstantValue<uint64_t>&>(const_val);
      member_value << value;
      break;
    }
    case flat::ConstantValue::Kind::kBool:
    case flat::ConstantValue::Kind::kFloat32:
    case flat::ConstantValue::Kind::kFloat64:
    case flat::ConstantValue::Kind::kString:
      assert(false && "bad primitive type for an enum");
      break;
  }

  *out_value = member_value.str();
}

flat::Decl::Kind GetDeclKind(const flat::Library* library, const flat::Type* type) {
  if (type->kind != flat::Type::Kind::kIdentifier)
    return flat::Decl::Kind::kConst;
  auto identifier_type = static_cast<const flat::IdentifierType*>(type);
  auto named_decl = library->LookupDeclByName(identifier_type->name);
  assert(named_decl && "library must contain declaration");
  return named_decl->kind;
}

void ArrayCountsAndElementTypeName(const flat::Library* library, const flat::Type* type,
                                   std::vector<uint32_t>* out_array_counts,
                                   std::string* out_element_type_name) {
  std::vector<uint32_t> array_counts;
  for (;;) {
    switch (type->kind) {
      default: {
        *out_element_type_name = NameFlatCType(type, GetDeclKind(library, type), WireFormat::kOld);
        *out_array_counts = array_counts;
        return;
      }
      case flat::Type::Kind::kArray: {
        auto array_type = static_cast<const flat::ArrayType*>(type);
        array_counts.push_back(array_type->element_count->value);
        type = array_type->element_type;
        continue;
      }
    }
  }
}

template <typename T>
CGenerator::Member CreateMember(const flat::Library* library, const T& decl) {
  std::string name = NameIdentifier(decl.name);
  const flat::Type* type = decl.type_ctor->type;
  auto decl_kind = GetDeclKind(library, type);
  auto type_name = NameFlatCType(type, decl_kind, WireFormat::kOld);
  std::string element_type_name;
  std::vector<uint32_t> array_counts;
  types::Nullability nullability = types::Nullability::kNonnullable;
  uint32_t max_num_elements = std::numeric_limits<uint32_t>::max();
  switch (type->kind) {
    case flat::Type::Kind::kArray: {
      ArrayCountsAndElementTypeName(library, type, &array_counts, &element_type_name);
      break;
    }
    case flat::Type::Kind::kVector: {
      auto vector_type = static_cast<const flat::VectorType*>(type);
      const auto element_type = vector_type->element_type;
      element_type_name =
          NameFlatCType(element_type, GetDeclKind(library, element_type), WireFormat::kOld);
      max_num_elements = vector_type->element_count->value;
      break;
    }
    case flat::Type::Kind::kIdentifier: {
      auto identifier_type = static_cast<const flat::IdentifierType*>(type);
      nullability = identifier_type->nullability;
      break;
    }
    case flat::Type::Kind::kString: {
      auto string_type = static_cast<const flat::StringType*>(type);
      nullability = string_type->nullability;
      max_num_elements = string_type->max_size->value;
      break;
    }
    case flat::Type::Kind::kHandle:
      break;
    case flat::Type::Kind::kRequestHandle:
      break;
    case flat::Type::Kind::kPrimitive:
      break;
  }
  return CGenerator::Member{
      type->kind,
      decl_kind,
      std::move(type_name),
      std::move(name),
      std::move(element_type_name),
      std::move(array_counts),
      nullability,
      max_num_elements,
  };
}

std::vector<CGenerator::Member> GenerateMembers(
    const flat::Library* library, const std::vector<flat::Union::Member>& union_members) {
  std::vector<CGenerator::Member> members;
  members.reserve(union_members.size());
  for (const auto& union_member : union_members) {
    if (union_member.maybe_used) {
      members.push_back(CreateMember(library, *union_member.maybe_used));
    }
  }
  return members;
}

std::vector<CGenerator::Member> GenerateMembers(
    const flat::Library* library, const std::vector<flat::XUnion::Member>& xunion_members) {
  std::vector<CGenerator::Member> members;
  members.reserve(xunion_members.size());
  for (const auto& xunion_member : xunion_members) {
    if (xunion_member.maybe_used) {
      members.push_back(CreateMember(library, *xunion_member.maybe_used));
    }
  }
  return members;
}

void GetMethodParameters(const flat::Library* library, const CGenerator::NamedMethod& method_info,
                         std::vector<CGenerator::Member>* request,
                         std::vector<CGenerator::Member>* response) {
  if (request) {
    request->reserve(method_info.request->parameters.size());
    for (const auto& parameter : method_info.request->parameters) {
      request->push_back(CreateMember(library, parameter));
    }
  }

  if (response && method_info.response) {
    response->reserve(method_info.response->parameters.size());
    for (const auto& parameter : method_info.response->parameters) {
      response->push_back(CreateMember(library, parameter));
    }
  }
}

}  // namespace

uint32_t CGenerator::GetMaxHandlesFor(Transport transport, const TypeShape& typeshape) {
  switch (transport) {
    case Transport::Channel:
      return std::min(ZX_CHANNEL_MAX_MSG_HANDLES, typeshape.MaxHandles());
  }
  assert(false && "what transport?");
  return 0u;
}

void CGenerator::GeneratePrologues() {
  EmitFileComment(&file_);
  EmitHeaderGuard(&file_);
  EmitBlank(&file_);
  EmitIncludeHeader(&file_, "<stdalign.h>");
  EmitIncludeHeader(&file_, "<stdbool.h>");
  EmitIncludeHeader(&file_, "<stdint.h>");
  EmitIncludeHeader(&file_, "<zircon/fidl.h>");
  EmitIncludeHeader(&file_, "<zircon/syscalls/object.h>");
  EmitIncludeHeader(&file_, "<zircon/types.h>");
  // Dependencies are in pointer order... change to a deterministic
  // ordering prior to output.
  std::set<std::string> add_includes;
  for (const auto& dep_library : library_->dependencies()) {
    if (dep_library == library_)
      continue;
    if (dep_library->HasAttribute("Internal"))
      continue;
    add_includes.insert(NameLibraryCHeader(dep_library->name()));
  }
  for (const auto& include : add_includes) {
    EmitIncludeHeader(&file_, "<" + include + ">");
  }
  EmitBlank(&file_);
  EmitBeginExternC(&file_);
  EmitBlank(&file_);
}

void CGenerator::GenerateEpilogues() { EmitEndExternC(&file_); }

void CGenerator::GenerateIntegerDefine(std::string_view name, types::PrimitiveSubtype subtype,
                                       std::string_view value) {
  std::string literal_macro = NamePrimitiveIntegerCConstantMacro(subtype);
  file_ << "#define " << name << " " << literal_macro << "(" << value << ")\n";
}

void CGenerator::GeneratePrimitiveDefine(std::string_view name, types::PrimitiveSubtype subtype,
                                         std::string_view value) {
  switch (subtype) {
    case types::PrimitiveSubtype::kInt8:
    case types::PrimitiveSubtype::kInt16:
    case types::PrimitiveSubtype::kInt32:
    case types::PrimitiveSubtype::kInt64:
    case types::PrimitiveSubtype::kUint8:
    case types::PrimitiveSubtype::kUint16:
    case types::PrimitiveSubtype::kUint32:
    case types::PrimitiveSubtype::kUint64: {
      std::string literal_macro = NamePrimitiveIntegerCConstantMacro(subtype);
      file_ << "#define " << name << " " << literal_macro << "(" << value << ")\n";
      break;
    }
    case types::PrimitiveSubtype::kBool:
    case types::PrimitiveSubtype::kFloat32:
    case types::PrimitiveSubtype::kFloat64: {
      file_ << "#define " << name << " "
            << "(" << value << ")\n";
      break;
    }
  }  // switch
}

void CGenerator::GenerateStringDefine(std::string_view name, std::string_view value) {
  file_ << "#define " << name << " " << value << "\n";
}

void CGenerator::GenerateIntegerTypedef(types::PrimitiveSubtype subtype, std::string_view name) {
  std::string underlying_type = NamePrimitiveCType(subtype);
  file_ << "typedef " << underlying_type << " " << name << ";\n";
}

void CGenerator::GenerateStructTypedef(std::string_view name) {
  file_ << "typedef struct " << name << " " << name << ";\n";
}

void CGenerator::GenerateStructDeclaration(std::string_view name,
                                           const std::vector<Member>& members, StructKind kind) {
  bool should_recursive_deprecate = ShouldRecursiveDeprecate(members);
  file_ << "struct " << (should_recursive_deprecate ? "__attribute__ ((deprecated)) " : "") << name
        << " {\n";

  if (kind == StructKind::kMessage) {
    file_ << kIndent << "FIDL_ALIGNDECL\n";
  }

  auto emit_member = [this](const Member& member) {
    file_ << kIndent;
    EmitMemberDecl(&file_, member);
    file_ << ";\n";
  };

  for (const auto& member : members) {
    emit_member(member);
  }

  if (members.empty()) {
    emit_member(EmptyStructMember());
  }

  file_ << "};\n";
}

void CGenerator::GenerateTableDeclaration(std::string_view name) {
  file_ << "struct " << name << " {\n";
  file_ << kIndent << "fidl_table_t table_header;\n";
  file_ << "};\n";
}

void CGenerator::GenerateTaggedUnionDeclaration(std::string_view name,
                                                const std::vector<Member>& members) {
#ifdef FIDLC_DEPRECATE_C_UNIONS
  file_ << "struct __attribute__ ((deprecated)) " << name << " {\n";
#else
  file_ << "struct " << name << " {\n";
#endif
  file_ << kIndent << "fidl_union_tag_t tag;\n";
  file_ << kIndent << "union {\n";
  for (const auto& member : members) {
    file_ << kIndent << kIndent;
    EmitMemberDecl(&file_, member);
    file_ << ";\n";
  }
  file_ << kIndent << "};\n";
  file_ << "};\n";
}

void CGenerator::GenerateTaggedXUnionDeclaration(std::string_view name,
                                                 const std::vector<Member>& members) {
  file_ << "struct " << name << " {\n";
  file_ << kIndent << "fidl_xunion_t xunion_header;\n";
  file_ << "};\n";
}

std::map<const flat::Decl*, CGenerator::NamedBits> CGenerator::NameBits(
    const std::vector<std::unique_ptr<flat::Bits>>& bits_infos) {
  std::map<const flat::Decl*, NamedBits> named_bits;
  for (const auto& bits_info : bits_infos) {
    std::string bits_name = NameCodedName(bits_info->name, WireFormat::kOld);
    named_bits.emplace(bits_info.get(), NamedBits{std::move(bits_name), *bits_info});
  }
  return named_bits;
}

// TODO(TO-702) These should maybe check for global name
// collisions? Otherwise, is there some other way they should fail?
std::map<const flat::Decl*, CGenerator::NamedConst> CGenerator::NameConsts(
    const std::vector<std::unique_ptr<flat::Const>>& const_infos) {
  std::map<const flat::Decl*, NamedConst> named_consts;
  for (const auto& const_info : const_infos) {
    named_consts.emplace(
        const_info.get(),
        NamedConst{NameCodedName(const_info->name, WireFormat::kOld), *const_info});
  }
  return named_consts;
}

std::map<const flat::Decl*, CGenerator::NamedEnum> CGenerator::NameEnums(
    const std::vector<std::unique_ptr<flat::Enum>>& enum_infos) {
  std::map<const flat::Decl*, NamedEnum> named_enums;
  for (const auto& enum_info : enum_infos) {
    std::string enum_name = NameCodedName(enum_info->name, WireFormat::kOld);
    named_enums.emplace(enum_info.get(), NamedEnum{std::move(enum_name), *enum_info});
  }
  return named_enums;
}

std::map<const flat::Decl*, CGenerator::NamedProtocol> CGenerator::NameProtocols(
    const std::vector<std::unique_ptr<flat::Protocol>>& protocol_infos) {
  std::map<const flat::Decl*, NamedProtocol> named_protocols;
  for (const auto& protocol_info : protocol_infos) {
    NamedProtocol named_protocol;
    named_protocol.c_name = NameCodedName(protocol_info->name, WireFormat::kOld);
    std::string alt_c_name = NameCodedName(protocol_info->name, WireFormat::kV1NoEe);
    if (protocol_info->HasAttribute("Discoverable")) {
      named_protocol.discoverable_name = NameDiscoverable(*protocol_info);
    }
    named_protocol.transport = ParseTransport(protocol_info->GetAttribute("Transport"));
    for (const auto& method_with_info : protocol_info->all_methods) {
      assert(method_with_info.method != nullptr);
      const auto& method = *method_with_info.method;
      NamedMethod named_method;
      std::string method_name = NameMethod(named_protocol.c_name, method);
      std::string alt_method_name = NameMethod(alt_c_name, method);
      named_method.ordinal = static_cast<uint64_t>(method.generated_ordinal32->value) << 32;
      named_method.ordinal_name = NameOrdinal(method_name);
      named_method.generated_ordinal = static_cast<uint64_t>(method.generated_ordinal64->value);
      named_method.generated_ordinal_name = NameGenOrdinal(method_name);
      named_method.identifier = NameIdentifier(method.name);
      named_method.c_name = method_name;
      if (method.maybe_request != nullptr) {
        std::string c_name = NameMessage(method_name, types::MessageKind::kRequest);
        std::string coded_name = NameTable(c_name);
        std::string alt_coded_name =
            NameTable(NameMessage(alt_method_name, types::MessageKind::kRequest));
        named_method.request = std::make_unique<NamedMessage>(NamedMessage{
            std::move(c_name), std::move(coded_name), std::move(alt_coded_name),
            method.maybe_request->members, method.maybe_request->typeshape(WireFormat::kOld),
            method.maybe_request->typeshape(WireFormat::kV1NoEe)});
      }
      if (method.maybe_response != nullptr) {
        auto message_kind = method.maybe_request ? types::MessageKind::kResponse
                                                 : types::MessageKind::kEvent;
        std::string c_name = NameMessage(method_name, message_kind);
        std::string coded_name = NameTable(c_name);
        std::string alt_coded_name =
            NameTable(NameMessage(alt_method_name, message_kind));
        named_method.response = std::make_unique<NamedMessage>(NamedMessage{
            std::move(c_name), std::move(coded_name), std::move(alt_coded_name),
            method.maybe_response->members, method.maybe_response->typeshape(WireFormat::kOld),
            method.maybe_response->typeshape(WireFormat::kV1NoEe)});
      }
      named_protocol.methods.push_back(std::move(named_method));
    }
    named_protocols.emplace(protocol_info.get(), std::move(named_protocol));
  }
  return named_protocols;
}

std::map<const flat::Decl*, CGenerator::NamedStruct> CGenerator::NameStructs(
    const std::vector<std::unique_ptr<flat::Struct>>& struct_infos) {
  std::map<const flat::Decl*, NamedStruct> named_structs;
  for (const auto& struct_info : struct_infos) {
    if (struct_info->is_request_or_response)
      continue;
    std::string c_name = NameCodedName(struct_info->name, WireFormat::kOld);
    std::string coded_name = c_name + "Coded";
    named_structs.emplace(struct_info.get(),
                          NamedStruct{std::move(c_name), std::move(coded_name), *struct_info});
  }
  return named_structs;
}

std::map<const flat::Decl*, CGenerator::NamedTable> CGenerator::NameTables(
    const std::vector<std::unique_ptr<flat::Table>>& table_infos) {
  std::map<const flat::Decl*, NamedTable> named_tables;
  for (const auto& table_info : table_infos) {
    std::string c_name = NameCodedName(table_info->name, WireFormat::kOld);
    std::string coded_name = c_name + "Coded";
    named_tables.emplace(table_info.get(),
                         NamedTable{std::move(c_name), std::move(coded_name), *table_info});
  }
  return named_tables;
}

std::map<const flat::Decl*, CGenerator::NamedUnion> CGenerator::NameUnions(
    const std::vector<std::unique_ptr<flat::Union>>& union_infos) {
  std::map<const flat::Decl*, NamedUnion> named_unions;
  for (const auto& union_info : union_infos) {
    std::string union_name = NameCodedName(union_info->name, WireFormat::kOld);
    if (!CUnionAllowed(union_name)) {
      continue;
    }
    named_unions.emplace(union_info.get(), NamedUnion{std::move(union_name), *union_info});
  }
  return named_unions;
}

std::map<const flat::Decl*, CGenerator::NamedXUnion> CGenerator::NameXUnions(
    const std::vector<std::unique_ptr<flat::XUnion>>& xunion_infos) {
  std::map<const flat::Decl*, NamedXUnion> named_xunions;
  for (const auto& xunion_info : xunion_infos) {
    std::string xunion_name = NameCodedName(xunion_info->name, WireFormat::kOld);
    named_xunions.emplace(xunion_info.get(), NamedXUnion{std::move(xunion_name), *xunion_info});
  }
  return named_xunions;
}

void CGenerator::ProduceBitsForwardDeclaration(const NamedBits& named_bits) {
  auto subtype =
      static_cast<const flat::PrimitiveType*>(named_bits.bits_info.subtype_ctor->type)->subtype;
  GenerateIntegerTypedef(subtype, named_bits.name);
  for (const auto& member : named_bits.bits_info.members) {
    std::string member_name = named_bits.name + "_" + NameIdentifier(member.name);
    std::string member_value;
    BitsValue(member.value.get(), &member_value);
    GenerateIntegerDefine(member_name, subtype, std::move(member_value));
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceConstForwardDeclaration(const NamedConst& named_const) {
  // TODO(TO-702)
}

void CGenerator::ProduceEnumForwardDeclaration(const NamedEnum& named_enum) {
  types::PrimitiveSubtype subtype = named_enum.enum_info.type->subtype;
  GenerateIntegerTypedef(subtype, named_enum.name);
  for (const auto& member : named_enum.enum_info.members) {
    std::string member_name = named_enum.name + "_" + NameIdentifier(member.name);
    std::string member_value;
    EnumValue(member.value.get(), &member_value);
    GenerateIntegerDefine(member_name, subtype, std::move(member_value));
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceProtocolForwardDeclaration(const NamedProtocol& named_protocol) {
  if (!named_protocol.discoverable_name.empty()) {
    file_ << "#define " << named_protocol.c_name << "_Name \"" << named_protocol.discoverable_name
          << "\"\n";
  }
  for (const auto& method_info : named_protocol.methods) {
    {
      IOFlagsGuard reset_flags(&file_);
      file_ << "#define " << method_info.ordinal_name << " ((uint64_t)0x" << std::uppercase
            << std::hex << method_info.ordinal << std::dec << ")\n";
      file_ << "#define " << method_info.generated_ordinal_name << " ((uint64_t)0x"
            << std::uppercase << std::hex << method_info.generated_ordinal << std::dec << ")\n";
    }
    if (method_info.request)
      GenerateStructTypedef(method_info.request->c_name);
    if (method_info.response)
      GenerateStructTypedef(method_info.response->c_name);
  }
}

void CGenerator::ProduceStructForwardDeclaration(const NamedStruct& named_struct) {
  GenerateStructTypedef(named_struct.c_name);
}

void CGenerator::ProduceTableForwardDeclaration(const NamedTable& named_struct) {
  GenerateStructTypedef(named_struct.c_name);
}

void CGenerator::ProduceTableDeclaration(const NamedTable& named_table) {
  GenerateTableDeclaration(named_table.c_name);
  EmitBlank(&file_);
}

void CGenerator::ProduceUnionForwardDeclaration(const NamedUnion& named_union) {
  GenerateStructTypedef(named_union.name);
}

void CGenerator::ProduceXUnionForwardDeclaration(const NamedXUnion& named_xunion) {
  GenerateStructTypedef(named_xunion.name);
}

void CGenerator::ProduceProtocolExternDeclaration(const NamedProtocol& named_protocol) {
  for (const auto& method_info : named_protocol.methods) {
    if (method_info.request) {
      file_ << "extern const fidl_type_t " << method_info.request->coded_name << ";\n";
      if (method_info.request->typeshape.ContainsUnion()) {
        file_ << "extern const fidl_type_t " << method_info.request->alt_coded_name << ";\n";
      }
    }
    if (method_info.response) {
      file_ << "extern const fidl_type_t " << method_info.response->coded_name << ";\n";
      if (method_info.response->typeshape.ContainsUnion()) {
        file_ << "extern const fidl_type_t " << method_info.response->alt_coded_name << ";\n";
      }
    }
  }
}

void CGenerator::ProduceConstDeclaration(const NamedConst& named_const) {
  const flat::Const& ci = named_const.const_info;

  // Some constants are not literals.  Odd.
  if (ci.value->kind != flat::Constant::Kind::kLiteral) {
    return;
  }

  switch (ci.type_ctor->type->kind) {
    case flat::Type::Kind::kPrimitive:
      GeneratePrimitiveDefine(
          named_const.name, static_cast<const flat::PrimitiveType*>(ci.type_ctor->type)->subtype,
          static_cast<flat::LiteralConstant*>(ci.value.get())->literal->location().data());
      break;
    case flat::Type::Kind::kString:
      GenerateStringDefine(
          named_const.name,
          static_cast<flat::LiteralConstant*>(ci.value.get())->literal->location().data());
      break;
    default:
      abort();
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceMessageDeclaration(const NamedMessage& named_message) {
  // When we generate a request or response struct (i.e. messages), we must
  // both include the message header, and ensure the message is FIDL aligned.

  std::vector<CGenerator::Member> members;
  members.reserve(1 + named_message.parameters.size());
  members.push_back(MessageHeader());
  for (const auto& parameter : named_message.parameters) {
    members.push_back(CreateMember(library_, parameter));
  }

  GenerateStructDeclaration(named_message.c_name, members, StructKind::kMessage);

  EmitBlank(&file_);
}

void CGenerator::ProduceProtocolDeclaration(const NamedProtocol& named_protocol) {
  for (const auto& method_info : named_protocol.methods) {
    if (method_info.request)
      ProduceMessageDeclaration(*method_info.request);
    if (method_info.response)
      ProduceMessageDeclaration(*method_info.response);
  }
}

void CGenerator::ProduceStructDeclaration(const NamedStruct& named_struct) {
  std::vector<CGenerator::Member> members;
  members.reserve(named_struct.struct_info.members.size());
  for (const auto& struct_member : named_struct.struct_info.members) {
    members.push_back(CreateMember(library_, struct_member));
  }

  GenerateStructDeclaration(named_struct.c_name, members, StructKind::kNonmessage);

  EmitBlank(&file_);
}

void CGenerator::ProduceUnionDeclaration(const NamedUnion& named_union) {
  std::vector<CGenerator::Member> members =
      GenerateMembers(library_, named_union.union_info.members);
  GenerateTaggedUnionDeclaration(named_union.name, members);

  uint32_t tag = 0u;
  for (const auto& member : named_union.union_info.members) {
    if (member.maybe_used) {
      std::string tag_name = NameUnionTag(named_union.name, *member.maybe_used);
      auto union_tag_type = types::PrimitiveSubtype::kUint32;
      std::ostringstream value;
      value << tag;
      GenerateIntegerDefine(std::move(tag_name), union_tag_type, value.str());
      ++tag;
    }
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceXUnionDeclaration(const NamedXUnion& named_xunion) {
  std::vector<CGenerator::Member> members =
      GenerateMembers(library_, named_xunion.xunion_info.members);
  GenerateTaggedXUnionDeclaration(named_xunion.name, members);

  uint32_t tag = 0u;
  for (const auto& member : named_xunion.xunion_info.members) {
    if (member.maybe_used) {
      std::string tag_name = NameXUnionTag(named_xunion.name, *member.maybe_used);
      auto xunion_tag_type = types::PrimitiveSubtype::kUint32;
      std::ostringstream value;
      value << tag;
      GenerateIntegerDefine(std::move(tag_name), xunion_tag_type, value.str());
      ++tag;
    }
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceProtocolClientDeclaration(const NamedProtocol& named_protocol) {
  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request)
      continue;
    std::vector<Member> request;
    std::vector<Member> response;
    GetMethodParameters(library_, method_info, &request, &response);
    EmitClientMethodDecl(&file_, method_info.c_name, request, response);
    file_ << ";\n";
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceProtocolClientImplementation(const NamedProtocol& named_protocol) {
  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request)
      continue;
    std::vector<Member> request;
    std::vector<Member> response;
    GetMethodParameters(library_, method_info, &request, &response);

    size_t count = CountSecondaryObjects(request);
    size_t request_hcount =
        GetMaxHandlesFor(named_protocol.transport, method_info.request->typeshape);
    size_t response_hcount = 0;
    if (method_info.response) {
      response_hcount = GetMaxHandlesFor(named_protocol.transport, method_info.response->typeshape);
    }
    size_t max_hcount = std::max(request_hcount, response_hcount);

    bool has_padding = method_info.request->typeshape.HasPadding();
    bool encode_request = (count > 0) || (request_hcount > 0) || has_padding;

    EmitClientMethodDecl(&file_, method_info.c_name, request, response);
    file_ << " {\n";
    EmitParameterSizeValidation(&file_, request);
    file_ << kIndent << "uint32_t _wr_num_bytes = sizeof(" << method_info.request->c_name << ")";
    EmitMeasureInParams(&file_, request);
    file_ << ";\n";
    file_ << kIndent << "FIDL_ALIGNDECL char _wr_bytes[_wr_num_bytes];\n";
    file_ << kIndent << method_info.request->c_name << "* _request = ("
          << method_info.request->c_name << "*)_wr_bytes;\n";
    file_ << kIndent << "memset(_wr_bytes, 0, sizeof(_wr_bytes));\n";
    EmitTxnHeader(&file_, "_request", method_info.ordinal_name);
    EmitLinearizeMessage(&file_, "_request", "_wr_bytes", request);
    const char* handles_value = "NULL";
    if (max_hcount > 0) {
      file_ << kIndent << "zx_handle_t _handles[" << max_hcount << "];\n";
      handles_value = "_handles";
    }
    if (encode_request) {
      file_ << kIndent << "uint32_t _wr_num_handles = 0u;\n";
      file_ << kIndent << "zx_status_t _status = fidl_encode(&" << method_info.request->coded_name
            << ", _wr_bytes, _wr_num_bytes, " << handles_value << ", " << request_hcount
            << ", &_wr_num_handles, NULL);\n";
      file_ << kIndent << "if (_status != ZX_OK)\n";
      file_ << kIndent << kIndent << "return _status;\n";
    } else {
      file_ << kIndent << "// OPTIMIZED AWAY fidl_encode() of POD-only request\n";
    }
    if (!method_info.response) {
      switch (named_protocol.transport) {
        case Transport::Channel:
          if (encode_request) {
            file_ << kIndent << "return zx_channel_write(_channel, 0u, _wr_bytes, _wr_num_bytes, "
                  << handles_value << ", _wr_num_handles);\n";
          } else {
            file_ << kIndent
                  << "return zx_channel_write(_channel, 0u, _wr_bytes, _wr_num_bytes, NULL, 0);\n";
          }
          break;
      }
    } else {
      file_ << kIndent << "uint32_t _rd_num_bytes = sizeof(" << method_info.response->c_name << ")";
      EmitMeasureOutParams(&file_, response);
      file_ << ";\n";

      file_ << kIndent << "uint32_t _rd_num_bytes_max = _rd_num_bytes;\n";
      // If the response potentially contains a union, enlarge the receiving buffer to hold the
      // xunion version if present.
      if (method_info.response->typeshape.ContainsUnion()) {
        file_ << kIndent << "uint32_t _rd_num_bytes_alt = "
              << method_info.response->alt_typeshape.InlineSize() +
                     method_info.response->alt_typeshape.MaxOutOfLine();
        file_ << ";\n";
        file_ << kIndent
              << "if (_rd_num_bytes_max < _rd_num_bytes_alt) { _rd_num_bytes_max = "
                 "_rd_num_bytes_alt; }\n";
      }

      file_ << kIndent << "FIDL_ALIGNDECL uint8_t _rd_bytes_storage[_rd_num_bytes_max];\n";
      file_ << kIndent << "uint8_t* _rd_bytes = _rd_bytes_storage;\n";
      if (!response.empty())
        file_ << kIndent << method_info.response->c_name << "* _response = ("
              << method_info.response->c_name << "*)_rd_bytes;\n";
      switch (named_protocol.transport) {
        case Transport::Channel:
          file_ << kIndent << "zx_channel_call_args_t _args = {\n";
          file_ << kIndent << kIndent << ".wr_bytes = _wr_bytes,\n";
          file_ << kIndent << kIndent << ".wr_handles = " << handles_value << ",\n";
          file_ << kIndent << kIndent << ".rd_bytes = _rd_bytes,\n";
          file_ << kIndent << kIndent << ".rd_handles = " << handles_value << ",\n";
          file_ << kIndent << kIndent << ".wr_num_bytes = _wr_num_bytes,\n";
          if (encode_request) {
            file_ << kIndent << kIndent << ".wr_num_handles = _wr_num_handles,\n";
          } else {
            file_ << kIndent << kIndent << ".wr_num_handles = 0,\n";
          }
          file_ << kIndent << kIndent << ".rd_num_bytes = _rd_num_bytes_max,\n";
          file_ << kIndent << kIndent << ".rd_num_handles = " << response_hcount << ",\n";
          file_ << kIndent << "};\n";

          file_ << kIndent << "uint32_t _actual_num_bytes = 0u;\n";
          file_ << kIndent << "uint32_t _actual_num_handles = 0u;\n";
          if (encode_request) {
            file_ << kIndent;
          } else {
            file_ << kIndent << "zx_status_t ";
          }
          file_ << "_status = zx_channel_call(_channel, 0u, ZX_TIME_INFINITE, &_args, "
                   "&_actual_num_bytes, &_actual_num_handles);\n";
          break;
      }
      file_ << kIndent << "if (_status != ZX_OK)\n";
      file_ << kIndent << kIndent << "return _status;\n";

      // We check that we have enough capacity to copy out the parameters
      // before decoding the message so that we can close the handles
      // using |_handles| rather than trying to find them in the decoded
      // message.
      count = CountSecondaryObjects(response);
      has_padding = method_info.response->typeshape.HasPadding();
      bool decode_response = (count > 0) || (response_hcount > 0) || has_padding;
      if (count > 0u) {
        file_ << kIndent << "if ";
        if (count > 1u)
          file_ << "(";
        size_t i = 0;
        for (const auto& member : response) {
          if (member.kind == flat::Type::Kind::kVector) {
            if (i++ > 0u)
              file_ << " || ";
            file_ << "(_response->" << member.name << ".count > " << member.name << "_capacity)";
          } else if (member.kind == flat::Type::Kind::kString) {
            if (i++ > 0u)
              file_ << " || ";
            file_ << "(_response->" << member.name << ".size > " << member.name << "_capacity)";
          } else if (IsStoredOutOfLine(member)) {
            if (i++ > 0u)
              file_ << " || ";
            file_ << "((uintptr_t)_response->" << member.name << " == FIDL_ALLOC_PRESENT && out_"
                  << member.name << " == NULL)";
          }
        }
        if (count > 1u)
          file_ << ")";
        file_ << " {\n";
        if (response_hcount > 0) {
          file_ << kIndent << kIndent << "zx_handle_close_many(_handles, _actual_num_handles);\n";
        }
        file_ << kIndent << kIndent << "return ZX_ERR_BUFFER_TOO_SMALL;\n";
        file_ << kIndent << "}\n";
      }

      // Perform xunion -> union transformation if necessary.
      if (method_info.response->typeshape.ContainsUnion()) {
        file_ << kIndent << "uint8_t* _transformer_dest = NULL;\n";
        file_ << kIndent << "if (fidl_should_decode_union_from_xunion(&_response->hdr)) {\n";
        file_ << kIndent << kIndent << "_transformer_dest = alloca(_rd_num_bytes);\n";
        file_ << kIndent << kIndent << "_status = fidl_transform(FIDL_TRANSFORMATION_V1_TO_OLD, &"
              << method_info.response->alt_coded_name
              << ", _rd_bytes, _actual_num_bytes"
              << ", _transformer_dest, _rd_num_bytes, &_actual_num_bytes"
              << ", NULL);\n";
        file_ << kIndent << kIndent << "if (_status != ZX_OK) {\n";
        if (max_hcount > 0) {
          file_ << kIndent << kIndent << kIndent
                << "zx_handle_close_many(_handles, _actual_num_handles);\n";
        }
        file_ << kIndent << kIndent << kIndent << "return _status;\n";
        file_ << kIndent << kIndent << "}\n";
        file_ << kIndent << kIndent << "_rd_bytes = _transformer_dest;\n";
        file_ << kIndent << kIndent << "_response = (" << method_info.response->c_name
              << "*)_rd_bytes;\n";
        file_ << kIndent << "}\n";
      }

      if (decode_response) {
        // TODO(FIDL-162): Validate the response ordinal. C++ bindings also need to do that.
        switch (named_protocol.transport) {
          case Transport::Channel:
            file_ << kIndent << "_status = fidl_decode(&" << method_info.response->coded_name
                  << ", _rd_bytes, _actual_num_bytes, " << handles_value
                  << ", _actual_num_handles, NULL);\n";
            break;
        }
        file_ << kIndent << "if (_status != ZX_OK)\n";
        file_ << kIndent << kIndent << "return _status;\n";
      } else {
        file_ << kIndent << "// OPTIMIZED AWAY fidl_decode() of POD-only response\n";
      }

      for (const auto& member : response) {
        const auto& name = member.name;
        switch (member.kind) {
          case flat::Type::Kind::kArray:
            file_ << kIndent << "memcpy(out_" << name << ", _response->" << name << ", ";
            EmitArraySizeOf(&file_, member);
            file_ << ");\n";
            break;
          case flat::Type::Kind::kVector:
            file_ << kIndent << "memcpy(" << name << "_buffer, _response->" << name
                  << ".data, sizeof(*" << name << "_buffer) * _response->" << name << ".count);\n";
            file_ << kIndent << "*out_" << name << "_count = _response->" << name << ".count;\n";
            break;
          case flat::Type::Kind::kString:
            file_ << kIndent << "memcpy(" << name << "_buffer, _response->" << name
                  << ".data, _response->" << name << ".size);\n";
            file_ << kIndent << "*out_" << name << "_size = _response->" << name << ".size;\n";
            break;
          case flat::Type::Kind::kHandle:
          case flat::Type::Kind::kRequestHandle:
          case flat::Type::Kind::kPrimitive:
            file_ << kIndent << "*out_" << name << " = _response->" << name << ";\n";
            break;
          case flat::Type::Kind::kIdentifier:
            switch (member.decl_kind) {
              case flat::Decl::Kind::kConst:
              case flat::Decl::Kind::kService:
              case flat::Decl::Kind::kTypeAlias:
                assert(false && "bad decl kind for member");
                break;
              case flat::Decl::Kind::kBits:
              case flat::Decl::Kind::kEnum:
              case flat::Decl::Kind::kProtocol:
                file_ << kIndent << "*out_" << name << " = _response->" << name << ";\n";
                break;
              case flat::Decl::Kind::kTable:
                assert(false && "c-codegen for tables not yet implemented");
                break;
              case flat::Decl::Kind::kXUnion:
                assert(false && "c-codegen for extensible unions not yet implemented");
                break;
              case flat::Decl::Kind::kStruct:
              case flat::Decl::Kind::kUnion:
                switch (member.nullability) {
                  case types::Nullability::kNullable:
                    file_ << kIndent << "if (_response->" << name << ") {\n";
                    file_ << kIndent << kIndent << "*out_" << name << " = *(_response->" << name
                          << ");\n";
                    file_ << kIndent << "} else {\n";
                    // We don't have a great way of signaling that the optional response member
                    // was not in the message. That means these bindings aren't particularly
                    // useful when the client needs to extract that bit. The best we can do is
                    // zero out the value to make sure the client has defined behavior.
                    //
                    // In many cases, the response contains other information (e.g., a status code)
                    // that lets the client do something reasonable.
                    file_ << kIndent << kIndent << "memset(out_" << name << ", 0, sizeof(*out_"
                          << name << "));\n";
                    file_ << kIndent << "}\n";
                    break;
                  case types::Nullability::kNonnullable:
                    file_ << kIndent << "*out_" << name << " = _response->" << name << ";\n";
                    break;
                }
                break;
            }
            break;
        }
      }

      file_ << kIndent << "return ZX_OK;\n";
    }
    file_ << "}\n\n";
  }
}

void CGenerator::ProduceProtocolServerDeclaration(const NamedProtocol& named_protocol) {
  file_ << "typedef struct " << named_protocol.c_name << "_ops {\n";
  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request)
      continue;
    std::vector<Member> request;
    GetMethodParameters(library_, method_info, &request, nullptr);
    bool has_response = method_info.response != nullptr;
    file_ << kIndent;
    EmitServerMethodDecl(&file_, method_info.identifier, request, has_response);
    file_ << ";\n";
  }
  file_ << "} " << named_protocol.c_name << "_ops_t;\n\n";

  EmitServerDispatchDecl(&file_, named_protocol.c_name);
  file_ << ";\n";
  EmitServerTryDispatchDecl(&file_, named_protocol.c_name);
  file_ << ";\n\n";

  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request || !method_info.response)
      continue;
    std::vector<Member> response;
    GetMethodParameters(library_, method_info, nullptr, &response);
    EmitServerReplyDecl(&file_, method_info.c_name, response);
    file_ << ";\n";
  }

  EmitBlank(&file_);
}

void CGenerator::ProduceProtocolServerImplementation(const NamedProtocol& named_protocol) {
  EmitServerTryDispatchDecl(&file_, named_protocol.c_name);
  file_ << " {\n";
  file_ << kIndent << "if (msg->num_bytes < sizeof(fidl_message_header_t)) {\n";
  file_ << kIndent << kIndent << "zx_handle_close_many(msg->handles, msg->num_handles);\n";
  file_ << kIndent << kIndent << "return ZX_ERR_INVALID_ARGS;\n";
  file_ << kIndent << "}\n";
  file_ << kIndent << "zx_status_t status = ZX_OK;\n";
  file_ << kIndent << "fidl_message_header_t* hdr = (fidl_message_header_t*)msg->bytes;\n";
  EmitMagicNumberCheck(&file_);
  file_ << kIndent << "switch (hdr->ordinal) {\n";

  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request)
      continue;
    if (method_info.ordinal != method_info.generated_ordinal) {
      file_ << kIndent << "case " << method_info.generated_ordinal_name << ":\n";
    }
    file_ << kIndent << "case " << method_info.ordinal_name << ": {\n";
    file_ << kIndent << kIndent << "status = fidl_decode_msg(&" << method_info.request->coded_name
          << ", msg, NULL);\n";
    file_ << kIndent << kIndent << "if (status != ZX_OK)\n";
    file_ << kIndent << kIndent << kIndent << "break;\n";
    std::vector<Member> request;
    GetMethodParameters(library_, method_info, &request, nullptr);
    if (!request.empty())
      file_ << kIndent << kIndent << method_info.request->c_name << "* request = ("
            << method_info.request->c_name << "*)msg->bytes;\n";
    file_ << kIndent << kIndent << "status = (*ops->" << method_info.identifier << ")(ctx";
    for (const auto& member : request) {
      switch (member.kind) {
        case flat::Type::Kind::kArray:
        case flat::Type::Kind::kHandle:
        case flat::Type::Kind::kRequestHandle:
        case flat::Type::Kind::kPrimitive:
          file_ << ", request->" << member.name;
          break;
        case flat::Type::Kind::kVector:
          file_ << ", (" << member.element_type << "*)request->" << member.name << ".data"
                << ", request->" << member.name << ".count";
          break;
        case flat::Type::Kind::kString:
          file_ << ", request->" << member.name << ".data"
                << ", request->" << member.name << ".size";
          break;
        case flat::Type::Kind::kIdentifier:
          switch (member.decl_kind) {
            case flat::Decl::Kind::kConst:
            case flat::Decl::Kind::kService:
            case flat::Decl::Kind::kTypeAlias:
              assert(false && "bad decl kind for member");
              break;
            case flat::Decl::Kind::kBits:
            case flat::Decl::Kind::kEnum:
            case flat::Decl::Kind::kProtocol:
              file_ << ", request->" << member.name;
              break;
            case flat::Decl::Kind::kTable:
              assert(false && "c-codegen for tables not yet implemented");
              break;
            case flat::Decl::Kind::kXUnion:
              assert(false && "c-codegen for extensible unions not yet implemented");
              break;
            case flat::Decl::Kind::kStruct:
            case flat::Decl::Kind::kUnion:
              switch (member.nullability) {
                case types::Nullability::kNullable:
                  file_ << ", request->" << member.name;
                  break;
                case types::Nullability::kNonnullable:
                  file_ << ", &(request->" << member.name << ")";
                  break;
              }
              break;
          }
      }
    }
    if (method_info.response != nullptr)
      file_ << ", txn";
    file_ << ");\n";
    file_ << kIndent << kIndent << "break;\n";
    file_ << kIndent << "}\n";
  }
  file_ << kIndent << "default: {\n";
  file_ << kIndent << kIndent << "return ZX_ERR_NOT_SUPPORTED;\n";
  file_ << kIndent << "}\n";
  file_ << kIndent << "}\n";
  file_ << kIndent << "if ("
        << "status != ZX_OK && "
        << "status != ZX_ERR_STOP && "
        << "status != ZX_ERR_NEXT && "
        << "status != ZX_ERR_ASYNC) {\n";
  file_ << kIndent << kIndent << "return ZX_ERR_INTERNAL;\n";
  file_ << kIndent << "} else {\n";
  file_ << kIndent << kIndent << "return status;\n";
  file_ << kIndent << "}\n";
  file_ << "}\n\n";

  EmitServerDispatchDecl(&file_, named_protocol.c_name);
  file_ << " {\n";
  file_ << kIndent << "zx_status_t status = " << named_protocol.c_name
        << "_try_dispatch(ctx, txn, msg, ops);\n";
  file_ << kIndent << "if (status == ZX_ERR_NOT_SUPPORTED)\n";
  file_ << kIndent << kIndent << "zx_handle_close_many(msg->handles, msg->num_handles);\n";
  file_ << kIndent << "return status;\n";
  file_ << "}\n\n";

  for (const auto& method_info : named_protocol.methods) {
    if (!method_info.request || !method_info.response)
      continue;
    std::vector<Member> response;
    GetMethodParameters(library_, method_info, nullptr, &response);

    size_t hcount = GetMaxHandlesFor(named_protocol.transport, method_info.response->typeshape);

    EmitServerReplyDecl(&file_, method_info.c_name, response);
    file_ << " {\n";
    file_ << kIndent << "uint32_t _wr_num_bytes = sizeof(" << method_info.response->c_name << ")";
    EmitMeasureInParams(&file_, response);
    file_ << ";\n";
    file_ << kIndent << "char _wr_bytes[_wr_num_bytes];\n";
    file_ << kIndent << method_info.response->c_name << "* _response = ("
          << method_info.response->c_name << "*)_wr_bytes;\n";
    file_ << kIndent << "memset(_wr_bytes, 0, sizeof(_wr_bytes));\n";
    EmitTxnHeader(&file_, "_response", method_info.ordinal_name);
    EmitLinearizeMessage(&file_, "_response", "_wr_bytes", response);
    const char* handle_value = "NULL";
    if (hcount > 0) {
      file_ << kIndent << "zx_handle_t _handles[" << hcount << "];\n";
      handle_value = "_handles";
    }
    file_ << kIndent << "fidl_msg_t _msg = {\n";
    file_ << kIndent << kIndent << ".bytes = _wr_bytes,\n";
    file_ << kIndent << kIndent << ".handles = " << handle_value << ",\n";
    file_ << kIndent << kIndent << ".num_bytes = _wr_num_bytes,\n";
    file_ << kIndent << kIndent << ".num_handles = " << hcount << ",\n";
    file_ << kIndent << "};\n";
    bool has_padding = method_info.response->typeshape.HasPadding();
    bool encode_response = (hcount > 0) || CountSecondaryObjects(response) > 0 || has_padding;
    if (encode_response) {
      file_ << kIndent << "zx_status_t _status = fidl_encode_msg(&"
            << method_info.response->coded_name << ", &_msg, &_msg.num_handles, NULL);\n";
      file_ << kIndent << "if (_status != ZX_OK)\n";
      file_ << kIndent << kIndent << "return _status;\n";
    } else {
      file_ << kIndent << "// OPTIMIZED AWAY fidl_encode() of POD-only reply\n";
    }
    if (method_info.response->typeshape.ContainsUnion()) {
      file_ << kIndent << "if (fidl_global_get_should_write_union_as_xunion()) {\n";
      file_ << kIndent << kIndent << "uint32_t _transformer_dest_capacity = "
            << method_info.response->alt_typeshape.InlineSize() +
                   method_info.response->alt_typeshape.MaxOutOfLine()
            << ";\n";
      file_ << kIndent << kIndent << "uint8_t* _transformer_dest = "
            << "alloca(_transformer_dest_capacity);\n";
      file_ << kIndent << kIndent << "_status = fidl_transform(FIDL_TRANSFORMATION_OLD_TO_V1, &"
            << method_info.response->coded_name
            << ", _msg.bytes, _msg.num_bytes"
            << ", _transformer_dest, _transformer_dest_capacity, &_msg.num_bytes"
            << ", NULL);\n";
      file_ << kIndent << kIndent << "if (_status != ZX_OK) {\n";
      if (hcount > 0) {
        file_ << kIndent << kIndent << kIndent
              << "zx_handle_close_many(_msg.handles, _msg.num_handles);\n";
      }
      file_ << kIndent << kIndent << kIndent << "return _status;\n";
      file_ << kIndent << kIndent << "}\n";
      file_ << kIndent << kIndent << "_msg.bytes = _transformer_dest;\n";
      file_ << kIndent << kIndent
            << "((fidl_message_header_t*)_msg.bytes)->flags[0] |= "
               "FIDL_TXN_HEADER_UNION_FROM_XUNION_FLAG;\n";
      file_ << kIndent << "}\n";
    }
    file_ << kIndent << "return _txn->reply(_txn, &_msg);\n";
    file_ << "}\n\n";
  }
}

std::ostringstream CGenerator::ProduceHeader() {
  GeneratePrologues();

  std::map<const flat::Decl*, NamedBits> named_bits = NameBits(library_->bits_declarations_);
  std::map<const flat::Decl*, NamedConst> named_consts = NameConsts(library_->const_declarations_);
  std::map<const flat::Decl*, NamedEnum> named_enums = NameEnums(library_->enum_declarations_);
  std::map<const flat::Decl*, NamedProtocol> named_protocols =
      NameProtocols(library_->protocol_declarations_);
  std::map<const flat::Decl*, NamedStruct> named_structs =
      NameStructs(library_->struct_declarations_);
  std::map<const flat::Decl*, NamedTable> named_tables = NameTables(library_->table_declarations_);
  std::map<const flat::Decl*, NamedUnion> named_unions = NameUnions(library_->union_declarations_);
  std::map<const flat::Decl*, NamedXUnion> named_xunions =
      NameXUnions(library_->xunion_declarations_);

  file_ << "\n// Forward declarations\n\n";

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits: {
        auto iter = named_bits.find(decl);
        if (iter != named_bits.end()) {
          ProduceBitsForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kConst: {
        auto iter = named_consts.find(decl);
        if (iter != named_consts.end()) {
          ProduceConstForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kEnum: {
        auto iter = named_enums.find(decl);
        if (iter != named_enums.end()) {
          ProduceEnumForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kProtocol: {
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kService:
        // Do nothing.
        break;
      case flat::Decl::Kind::kStruct: {
        auto iter = named_structs.find(decl);
        if (iter != named_structs.end()) {
          ProduceStructForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kTable: {
        auto iter = named_tables.find(decl);
        if (iter != named_tables.end()) {
          ProduceTableForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kTypeAlias:
        // TODO(FIDL-483): Do more than nothing.
        break;
      case flat::Decl::Kind::kUnion: {
        auto iter = named_unions.find(decl);
        if (iter != named_unions.end()) {
          ProduceUnionForwardDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kXUnion: {
        auto iter = named_xunions.find(decl);
        if (iter != named_xunions.end()) {
          ProduceXUnionForwardDeclaration(iter->second);
        }
        break;
      }
    }  // switch
  }

  file_ << "\n// Extern declarations\n\n";

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits:
      case flat::Decl::Kind::kConst:
      case flat::Decl::Kind::kEnum:
      case flat::Decl::Kind::kService:
      case flat::Decl::Kind::kStruct:
      case flat::Decl::Kind::kTable:
      case flat::Decl::Kind::kTypeAlias:
      case flat::Decl::Kind::kUnion:
      case flat::Decl::Kind::kXUnion:
        // Only messages have extern fidl_type_t declarations.
        break;
      case flat::Decl::Kind::kProtocol: {
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolExternDeclaration(iter->second);
        }
        break;
      }
    }  // switch
  }

  file_ << "\n// Declarations\n\n";

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits:
        // Bits can be entirely forward declared, as they have no
        // dependencies other than standard headers.
        break;
      case flat::Decl::Kind::kConst: {
        auto iter = named_consts.find(decl);
        if (iter != named_consts.end()) {
          ProduceConstDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kEnum:
        // Enums can be entirely forward declared, as they have no
        // dependencies other than standard headers.
        break;
      case flat::Decl::Kind::kProtocol: {
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kService:
        // Do nothing.
        break;
      case flat::Decl::Kind::kStruct: {
        auto iter = named_structs.find(decl);
        if (iter != named_structs.end()) {
          ProduceStructDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kTable: {
        auto iter = named_tables.find(decl);
        if (iter != named_tables.end()) {
          ProduceTableDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kTypeAlias:
        // TODO(FIDL-483): Do more than nothing.
        break;
      case flat::Decl::Kind::kUnion: {
        auto iter = named_unions.find(decl);
        if (iter != named_unions.end()) {
          ProduceUnionDeclaration(iter->second);
        }
        break;
      }
      case flat::Decl::Kind::kXUnion: {
        auto iter = named_xunions.find(decl);
        if (iter != named_xunions.end()) {
          ProduceXUnionDeclaration(iter->second);
        }
        break;
      }
    }  // switch
  }

  file_ << "\n// Simple bindings \n\n";

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits:
      case flat::Decl::Kind::kConst:
      case flat::Decl::Kind::kEnum:
      case flat::Decl::Kind::kService:
      case flat::Decl::Kind::kStruct:
      case flat::Decl::Kind::kTable:
      case flat::Decl::Kind::kTypeAlias:
      case flat::Decl::Kind::kUnion:
      case flat::Decl::Kind::kXUnion:
        // Only protocols have client declarations.
        break;
      case flat::Decl::Kind::kProtocol: {
        if (!HasSimpleLayout(decl))
          break;
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolClientDeclaration(iter->second);
          ProduceProtocolServerDeclaration(iter->second);
        }
        break;
      }
    }  // switch
  }

  GenerateEpilogues();

  return std::move(file_);
}

std::ostringstream CGenerator::ProduceClient() {
  EmitFileComment(&file_);
  EmitIncludeHeader(&file_, "<lib/fidl/coding.h>");
  EmitIncludeHeader(&file_, "<lib/fidl/transformer.h>");
  EmitIncludeHeader(&file_, "<lib/fidl/txn_header.h>");
  EmitIncludeHeader(&file_, "<alloca.h>");
  EmitIncludeHeader(&file_, "<string.h>");
  EmitIncludeHeader(&file_, "<zircon/syscalls.h>");
  EmitIncludeHeader(&file_, "<" + NameLibraryCHeader(library_->name()) + ">");
  EmitBlank(&file_);

  std::map<const flat::Decl*, NamedProtocol> named_protocols =
      NameProtocols(library_->protocol_declarations_);

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits:
      case flat::Decl::Kind::kConst:
      case flat::Decl::Kind::kEnum:
      case flat::Decl::Kind::kService:
      case flat::Decl::Kind::kStruct:
      case flat::Decl::Kind::kTable:
      case flat::Decl::Kind::kTypeAlias:
      case flat::Decl::Kind::kUnion:
      case flat::Decl::Kind::kXUnion:
        // Only protocols have client implementations.
        break;
      case flat::Decl::Kind::kProtocol: {
        if (!HasSimpleLayout(decl))
          break;
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolClientImplementation(iter->second);
        }
        break;
      }
    }  // switch
  }

  return std::move(file_);
}

std::ostringstream CGenerator::ProduceServer() {
  EmitFileComment(&file_);
  EmitIncludeHeader(&file_, "<lib/fidl/coding.h>");
  EmitIncludeHeader(&file_, "<lib/fidl/runtime_flag.h>");
  EmitIncludeHeader(&file_, "<lib/fidl/transformer.h>");
  EmitIncludeHeader(&file_, "<lib/fidl/txn_header.h>");
  EmitIncludeHeader(&file_, "<alloca.h>");
  EmitIncludeHeader(&file_, "<string.h>");
  EmitIncludeHeader(&file_, "<zircon/syscalls.h>");
  EmitIncludeHeader(&file_, "<" + NameLibraryCHeader(library_->name()) + ">");
  EmitBlank(&file_);

  std::map<const flat::Decl*, NamedProtocol> named_protocols =
      NameProtocols(library_->protocol_declarations_);

  for (const auto* decl : library_->declaration_order_) {
    switch (decl->kind) {
      case flat::Decl::Kind::kBits:
      case flat::Decl::Kind::kConst:
      case flat::Decl::Kind::kEnum:
      case flat::Decl::Kind::kService:
      case flat::Decl::Kind::kStruct:
      case flat::Decl::Kind::kTable:
      case flat::Decl::Kind::kTypeAlias:
      case flat::Decl::Kind::kUnion:
      case flat::Decl::Kind::kXUnion:
        // Only protocols have client implementations.
        break;
      case flat::Decl::Kind::kProtocol: {
        if (!HasSimpleLayout(decl))
          break;
        auto iter = named_protocols.find(decl);
        if (iter != named_protocols.end()) {
          ProduceProtocolServerImplementation(iter->second);
        }
        break;
      }
    }  // switch
  }

  return std::move(file_);
}

}  // namespace fidl
