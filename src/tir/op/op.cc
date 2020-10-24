/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file tir/op/op.cc
 *
 *  Common operator definitions for ops in tir/op.h
 */

#include <tvm/runtime/registry.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/expr.h>
#include <tvm/tir/op.h>
#include <tvm/tir/op_attr_types.h>

#include <cmath>
// Centralized header for constant folders.
#include "../../arith/const_fold.h"
#include "../../target/datatype/registry.h"

namespace tvm {

using namespace tir;

// macro to register an unary op
#define TIR_REGISTER_PURE_UNARY_OP(OpName)                             \
  TVM_REGISTER_OP(OpName).set_num_inputs(1).set_attr<TCallEffectKind>( \
      "TCallEffectKind", Integer(CallEffectKind::kPure))

// macro to register an binary op
#define TIR_REGISTER_PURE_BINARY_OP(OpName)                            \
  TVM_REGISTER_OP(OpName).set_num_inputs(2).set_attr<TCallEffectKind>( \
      "TCallEffectKind", Integer(CallEffectKind::kPure))

runtime::DataType GetRuntimeDataType(const Type& type) {
  if (auto* n = type.as<PrimTypeNode>()) {
    return n->dtype;
  } else if (type.as<PointerTypeNode>()) {
    return DataType::Handle();
  } else if (IsVoidType(type)) {
    return DataType::Void();
  } else {
    LOG(FATAL) << "Type " << type << " does not have a corresponding runtime::DataType";
    return DataType::Handle();
  }
}

Type GetType(const PrimExpr& expr) {
  // TODO(tqchen): add recursive type inference for Call here
  // once we introduced the corresponding fields to the IR.
  if (auto* ptr = expr.as<tir::VarNode>()) {
    // If Var has a more refined type annotation,
    // return the type anotation
    if (ptr->type_annotation.defined()) {
      return ptr->type_annotation;
    }
  }
  // Default: return the type indicated by the dtype.
  runtime::DataType dtype = expr.dtype();
  if (dtype.is_void()) {
    return VoidType();
  }
  return PrimType(dtype);
}

// simple cast that only checks if type matches and cast
inline PrimExpr SimpleCast(const DataType& t, PrimExpr value) {
  if (value.dtype() == t) return value;
  return tir::Cast(t, value);
}

// LargeUIntImm
PrimExpr LargeUIntImm(DataType t, int64_t low, int64_t high) {
  return tir::Call(t, tir::builtin::large_uint_imm(),
                   {make_const(DataType::UInt(32), low), make_const(DataType::UInt(32), high)});
}

// Q-multiplication
PrimExpr q_multiply_shift(PrimExpr x, PrimExpr y, PrimExpr q, PrimExpr s) {
  return tir::Call(DataType::Int(32, x.dtype().lanes()), tir::builtin::q_multiply_shift(),
                   {x, y, q, s});
}

// The public function with a quick checking path.
void BinaryOpMatchTypes(PrimExpr& lhs, PrimExpr& rhs) {  // NOLINT(*)
  if (lhs.dtype() == rhs.dtype()) return;
  DataType ltype = lhs.dtype();
  DataType rtype = rhs.dtype();
  if (ltype.lanes() == 1 && rtype.lanes() != 1) {
    lhs = tir::Broadcast(lhs, rtype.lanes());
  } else if (rtype.lanes() == 1 && ltype.lanes() != 1) {
    rhs = tir::Broadcast(rhs, ltype.lanes());
  } else {
    ICHECK(ltype.lanes() == rtype.lanes()) << "Cannot match type " << ltype << " vs " << rtype;
  }
  if (lhs.dtype() == rhs.dtype()) return;
  // Only do very simple type coversion
  // int->float, DataType::Int(32)->int(64)
  // require the types to be relatively consistent
  // This will the reduce amount code generated by operators
  // and also help user to find potential type conversion problems.
  if (!lhs.dtype().is_float() &&
      (rhs.dtype().is_float() ||
       datatype::Registry::Global()->GetTypeRegistered(rhs.dtype().code()))) {
    // int->float
    lhs = cast(rhs.dtype(), lhs);
  } else if ((lhs.dtype().is_float() ||
              datatype::Registry::Global()->GetTypeRegistered(lhs.dtype().code())) &&
             !rhs.dtype().is_float()) {
    // int->float
    rhs = cast(lhs.dtype(), rhs);
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_int()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_uint())) {
    // promote int to higher bits
    if (lhs.dtype().bits() < rhs.dtype().bits()) {
      lhs = cast(rhs.dtype(), lhs);
    } else {
      rhs = cast(lhs.dtype(), rhs);
    }
  } else if ((lhs.dtype().is_int() && rhs.dtype().is_uint()) ||
             (lhs.dtype().is_uint() && rhs.dtype().is_int())) {
    int bits = std::max(lhs.dtype().bits(), rhs.dtype().bits());
    lhs = SimpleCast(DataType::Int(bits, lhs.dtype().lanes()), lhs);
    rhs = SimpleCast(DataType::Int(bits, rhs.dtype().lanes()), rhs);
  } else {
    LOG(FATAL) << "Cannot match type " << ltype << " vs " << rtype;
  }
}

// maximum and min limits
PrimExpr max_value(const DataType& dtype) {
  using namespace tir;
  ICHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImm(dtype, std::numeric_limits<int64_t>::max());
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = (val << (dtype.bits() - 1)) - 1;
      return IntImm(dtype, val);
    }
  } else if (dtype.is_uint()) {
    if (dtype.bits() == 64) {
      return make_const(dtype, std::numeric_limits<uint64_t>::max());
    } else if (dtype.bits() < 64) {
      uint64_t val = 1;
      val = (val << static_cast<uint64_t>(dtype.bits())) - 1;
      return IntImm(dtype, static_cast<int64_t>(val));
    }
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::max());
    } else if (dtype.bits() == 32) {
      return FloatImm(dtype, std::numeric_limits<float>::max());
    } else if (dtype.bits() == 16) {
      return FloatImm(dtype, 65504.0);
    }
  }
  LOG(FATAL) << "Cannot decide max_value for type" << dtype;
  return PrimExpr();
}

PrimExpr min_value(const DataType& dtype) {
  using namespace tir;
  ICHECK_EQ(dtype.lanes(), 1);
  if (datatype::Registry::Global()->GetTypeRegistered(dtype.code())) {
    auto f = datatype::GetMinFunc(dtype.code());
    ICHECK(f) << "No minimum function registered for custom dtype " << (unsigned int)dtype.code();
    // TODO(@hypercubestart) Document this change (and others associated with the overflowing
    // floatimm min bug)
    return (*f)(dtype.bits());
  } else if (dtype.is_int()) {
    if (dtype.bits() == 64) {
      return IntImm(dtype, std::numeric_limits<int64_t>::lowest());
    } else if (dtype.bits() < 64) {
      int64_t val = 1;
      val = -(val << (dtype.bits() - 1));
      return IntImm(dtype, val);
    }
  } else if (dtype.is_uint()) {
    return IntImm(dtype, 0);
  } else if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::lowest());
    } else if (dtype.bits() == 32) {
      return FloatImm(dtype, std::numeric_limits<float>::lowest());
    } else if (dtype.bits() == 16) {
      return FloatImm(dtype, -65504.0);
    }
  }
  LOG(FATAL) << "Cannot decide min_value for type" << dtype;
  return PrimExpr();
}

// infinity
PrimExpr infinity(const DataType& dtype) {
  using namespace tir;
  ICHECK_EQ(dtype.lanes(), 1);
  if (dtype.is_float()) {
    if (dtype.bits() == 64) {
      return FloatImm(dtype, std::numeric_limits<double>::infinity());
    } else if (dtype.bits() == 32 || dtype.bits() == 16) {
      return FloatImm(dtype, std::numeric_limits<float>::infinity());
    }
  }
  LOG(FATAL) << "Cannot decide infinity for type " << dtype;
  return PrimExpr();
}

namespace tir {
template <typename ValueType>
inline bool ConstPowerHelper(ValueType val, int* shift) {
  if (val <= 0) return false;
  shift[0] = 0;
  while (val != 0) {
    if (val & 1) {
      return (val == 1);
    }
    ++shift[0];
    val = val >> 1;
  }
  return true;
}

bool is_const_power_of_two_integer(const PrimExpr& x, int* shift) {
  if (const auto* op = x.as<tir::IntImmNode>()) {
    return ConstPowerHelper(op->value, shift);
  } else {
    return false;
  }
}
}  // namespace tir

PrimExpr cast(const DataType& t, PrimExpr value) {
  using tir::FloatImmNode;
  if (value.dtype() == t) return value;
  // const fold IntImm as they are used in index computations
  if (t.lanes() == 1) {
    if (const IntImmNode* op = value.as<IntImmNode>()) {
      return make_const(t, op->value);
    } else if (const FloatImmNode* op = value.as<FloatImmNode>()) {
      return make_const(t, op->value);
    }
    return tir::Cast(t, value);
  } else {
    if (value.dtype().lanes() == 1) {
      // manually unroll cast
      DataType vtype = t.element_of();
      if (value.dtype() != vtype) {
        if (const IntImmNode* op = value.as<IntImmNode>()) {
          value = make_const(vtype, op->value);
        } else if (const FloatImmNode* op = value.as<FloatImmNode>()) {
          value = make_const(vtype, op->value);
        } else {
          value = tir::Cast(vtype, value);
        }
      }
      return tir::Broadcast(value, t.lanes());
    } else {
      ICHECK(value.dtype().lanes() == t.lanes());
      return tir::Cast(t, value);
    }
  }
}

// reinterpret
PrimExpr reinterpret(const DataType& t, PrimExpr value) {
  if (value.dtype() == t) return value;
  return tir::Call(t, tir::builtin::reinterpret(), {value});
}

// operator+
PrimExpr operator+(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Add>(a, b);
  if (ret.defined()) return ret;
  return tir::Add(a, b);
}

// negation
PrimExpr operator-(PrimExpr a) {
  using tir::FloatImmNode;
  using tir::IntImmNode;
  const IntImmNode* pa = a.as<IntImmNode>();
  const FloatImmNode* fa = a.as<FloatImmNode>();
  if (pa) return IntImm(a.dtype(), -pa->value);
  if (fa) return FloatImm(a.dtype(), -fa->value);
  return make_zero(a.dtype()) - a;
}

PrimExpr operator-(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Sub>(a, b);
  if (ret.defined()) return ret;
  return tir::Sub(a, b);
}

PrimExpr operator*(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Mul>(a, b);
  if (ret.defined()) return ret;
  return tir::Mul(a, b);
}

PrimExpr div(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Div>(a, b);
  if (ret.defined()) return ret;
  return tir::Div(a, b);
}

PrimExpr truncdiv(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  ICHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  return div(a, b);
}

PrimExpr truncmod(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Mod>(a, b);
  if (ret.defined()) return ret;
  return tir::Mod(a, b);
}

PrimExpr operator/(PrimExpr a, PrimExpr b) { return div(a, b); }

PrimExpr operator%(PrimExpr a, PrimExpr b) { return truncmod(a, b); }

// TODO(tqchen): switch to floordiv
PrimExpr indexdiv(PrimExpr a, PrimExpr b) { return floordiv(a, b); }

PrimExpr indexmod(PrimExpr a, PrimExpr b) { return floormod(a, b); }

PrimExpr floordiv(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  ICHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::FloorDiv>(a, b);
  if (ret.defined()) return ret;
  return tir::FloorDiv(a, b);
}

PrimExpr floormod(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint()) << a;
  ICHECK(b.dtype().is_int() || b.dtype().is_uint()) << b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::FloorMod>(a, b);
  if (ret.defined()) return ret;
  return tir::FloorMod(a, b);
}

PrimExpr min(PrimExpr a, PrimExpr b) {
  // inf-aware simplificaiton
  using arith::is_neg_inf;
  using arith::is_pos_inf;
  if (is_pos_inf(a)) return b;
  if (is_neg_inf(a)) return a;
  if (is_pos_inf(b)) return a;
  if (is_neg_inf(b)) return b;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Min>(a, b);
  if (ret.defined()) return ret;
  return tir::Min(a, b);
}

PrimExpr max(PrimExpr a, PrimExpr b) {
  // inf-aware simplificaiton
  using arith::is_neg_inf;
  using arith::is_pos_inf;
  if (is_pos_inf(a)) return a;
  if (is_neg_inf(a)) return b;
  if (is_pos_inf(b)) return b;
  if (is_neg_inf(b)) return a;
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::Max>(a, b);
  if (ret.defined()) return ret;
  return tir::Max(a, b);
}

// if_then_else
PrimExpr if_then_else(PrimExpr cond, PrimExpr true_value, PrimExpr false_value) {
  ICHECK(cond.dtype() == DataType::Bool(1))
      << "if_then_else only accept the condition to be boolean type.";
  BinaryOpMatchTypes(true_value, false_value);
  if (const IntImmNode* op = cond.as<IntImmNode>()) {
    if (op->value != 0) {
      return true_value;
    } else {
      return false_value;
    }
  }

  return tir::Call(true_value.dtype(), tir::builtin::if_then_else(),
                   {cond, true_value, false_value});
}

// likely
PrimExpr likely(PrimExpr cond) {
  if (is_const_int(cond)) return cond;
  return tir::Call(cond.dtype(), tir::builtin::likely(), {cond});
}

// operator>
PrimExpr operator>(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::GT>(a, b);
  if (ret.defined()) return ret;
  return tir::GT(a, b);
}

PrimExpr operator>=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::GE>(a, b);
  if (ret.defined()) return ret;
  return tir::GE(a, b);
}

PrimExpr operator<(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::LT>(a, b);
  if (ret.defined()) return ret;
  return tir::LT(a, b);
}

PrimExpr operator<=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::LE>(a, b);
  if (ret.defined()) return ret;
  return tir::LE(a, b);
}

PrimExpr operator==(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::EQ>(a, b);
  if (ret.defined()) return ret;
  return tir::EQ(a, b);
}

PrimExpr operator!=(PrimExpr a, PrimExpr b) {
  BinaryOpMatchTypes(a, b);
  PrimExpr ret = arith::TryConstFold<tir::NE>(a, b);
  if (ret.defined()) return ret;
  return tir::NE(a, b);
}

PrimExpr operator&&(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_bool());
  ICHECK(b.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<tir::And>(a, b);
  if (ret.defined()) return ret;
  return tir::And(a, b);
}

PrimExpr operator||(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_bool());
  ICHECK(b.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<tir::Or>(a, b);
  if (ret.defined()) return ret;
  return tir::Or(a, b);
}

PrimExpr operator!(PrimExpr a) {
  ICHECK(a.dtype().is_bool());
  PrimExpr ret = arith::TryConstFold<tir::Not>(a);
  if (ret.defined()) return ret;
  return tir::Not(a);
}

// shirt right
PrimExpr operator>>(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  ICHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pb)
      ICHECK(pb->value >= 0 && pb->value < rtype.bits())
          << "Shift amount must be non-negative and less than " << rtype.bits() << " for type "
          << rtype;
    if (pa && pb) return IntImm(rtype, (pa->value >> pb->value));
    if (pb) {
      if (pb->value == 0) return a;
    }
  });

  return tir::Call(a.dtype(), tir::builtin::shift_right(), {a, b});
}

// shift left
PrimExpr operator<<(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  ICHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pb)
      ICHECK(pb->value >= 0 && pb->value < rtype.bits())
          << "Shift amount must be non-negative and less than " << rtype.bits() << " for type "
          << rtype;
    if (pa && pb) return IntImm(rtype, (pa->value << pb->value));
    if (pb) {
      if (pb->value == 0) return a;
    }
  });
  return tir::Call(a.dtype(), tir::builtin::shift_left(), {a, b});
}

// bitwise and
PrimExpr operator&(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  ICHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb) return IntImm(rtype, (pa->value & pb->value));
  });
  return tir::Call(a.dtype(), tir::builtin::bitwise_and(), {a, b});
}

// bitwise_or
PrimExpr operator|(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  ICHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb) return IntImm(rtype, (pa->value | pb->value));
  });
  return tir::Call(a.dtype(), tir::builtin::bitwise_or(), {a, b});
}

// bitwise_xor
PrimExpr operator^(PrimExpr a, PrimExpr b) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  ICHECK(b.dtype().is_int() || b.dtype().is_uint());
  BinaryOpMatchTypes(a, b);
  TVM_INDEX_CONST_PROPAGATION({
    const DataType& rtype = a.dtype();
    if (pa && pb) return IntImm(rtype, (pa->value ^ pb->value));
  });
  return tir::Call(a.dtype(), tir::builtin::bitwise_xor(), {a, b});
}

// bitwie_not
PrimExpr operator~(PrimExpr a) {
  ICHECK(a.dtype().is_int() || a.dtype().is_uint());
  return tir::Call(a.dtype(), tir::builtin::bitwise_not(), {a});
}

TVM_REGISTER_GLOBAL("tir.bitwise_not").set_body_typed([](PrimExpr a) { return ~a; });

// pow
PrimExpr pow(PrimExpr x, PrimExpr y) {
  BinaryOpMatchTypes(x, y);
  ICHECK(x.dtype().is_float()) << "power only applies to float";
  static auto op = Op::Get("tir.pow");
  return tir::Call(x.dtype(), op, {x, y});
}

TIR_REGISTER_PURE_BINARY_OP("tir.pow").set_attr<TVectorizable>("TVectorizable", true);

// abs
PrimExpr abs(PrimExpr x) {
  if (x.dtype().is_int()) {
    using tir::IntImmNode;
    const IntImmNode* px = x.as<IntImmNode>();
    if (px) {
      return IntImm(x.dtype(), std::abs(px->value));
    }
    return tir::Select(x >= make_zero(x.dtype()), x, -x);
  } else if (x.dtype().is_float()) {
    using tir::FloatImmNode;
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return FloatImm(x.dtype(), std::fabs(fx->value));
    }
    static auto op = Op::Get("tir.fabs");
    return tir::Call(x.dtype(), op, {x});
  } else if (x.dtype().is_uint()) {
    return x;
  } else {
    LOG(FATAL) << "Data type " << x.dtype()
               << " not supported for absolute op. Skipping absolute op...";
    return x;
  }
}

TIR_REGISTER_PURE_UNARY_OP("tir.fabs").set_attr<TVectorizable>("TVectorizable", true);

// isnan
PrimExpr isnan(PrimExpr x) {
  DataType t = DataType::Bool(x.dtype().lanes());
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return make_const(t, false);
  } else if (x.dtype().is_float()) {
    using tir::FloatImmNode;
    const FloatImmNode* fx = x.as<FloatImmNode>();
    if (fx) {
      return make_const(t, std::isnan(fx->value));
    }
    static auto op = Op::Get("tir.isnan");
    if (x.dtype().bits() == 16) {
      return tir::Call(t, op, {cast(DataType::Float(32, t.lanes()), std::move(x))});
    } else {
      return tir::Call(t, op, {x});
    }
  } else {
    LOG(FATAL) << "Data type " << x.dtype() << " not supported for isnan op. Skipping isnan op...";
    return x;
  }
}

// isinf
PrimExpr isinf(PrimExpr x) {
  DataType t = DataType::Bool(x.dtype().lanes());
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return make_const(t, false);
  } else if (x.dtype().is_float()) {
    PrimExpr infX = infinity(x.dtype());
    return abs(x) == infX && !isnan(x);
  } else {
    LOG(FATAL) << "Data type " << x.dtype() << " not supported for finiteness ops. Skipping it...";
    return x;
  }
}

// isfinite
PrimExpr isfinite(PrimExpr x) { return !isinf(x) && !isnan(x); }

PrimExpr sum(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::Add(x, y);
  PrimExpr identity_element = make_zero(source.dtype());
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

PrimExpr all(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  ICHECK(source.dtype().is_bool());
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::And(x, y);
  PrimExpr identity_element = make_const(source.dtype(), true);
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

PrimExpr any(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  ICHECK(source.dtype().is_bool());
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::Or(x, y);
  PrimExpr identity_element = make_const(source.dtype(), false);
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

PrimExpr max(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::Max(x, y);
  PrimExpr identity_element = min_value(source.dtype());
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

PrimExpr min(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::Min(x, y);
  PrimExpr identity_element = max_value(source.dtype());
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

PrimExpr prod(PrimExpr source, Array<IterVar> rdom, Array<PrimExpr> init) {
  Var x("x", source.dtype()), y("y", source.dtype());
  PrimExpr result = tir::Mul(x, y);
  PrimExpr identity_element = make_const(source.dtype(), 1);
  tir::CommReducer combiner = tir::CommReducer({x}, {y}, {result}, {identity_element});
  return tir::Reduce(combiner, {source}, rdom, make_const(DataType::Bool(1), true), 0, init);
}

// fmod
PrimExpr fmod(PrimExpr x, PrimExpr y) {
  BinaryOpMatchTypes(x, y);
  ICHECK(x.dtype().is_float()) << "fmod only applies to float";
  static auto op = Op::Get("tir.fmod");
  return tir::Call(x.dtype(), op, {x, y});
}

TIR_REGISTER_PURE_UNARY_OP("tir.fmod");

// floor
PrimExpr floor(PrimExpr x) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  using tir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImm(x.dtype(), std::floor(fx->value));
  static auto op = Op::Get("tir.floor");
  return tir::Call(x.dtype(), op, {x});
}

TIR_REGISTER_PURE_UNARY_OP("tir.floor").set_attr<TVectorizable>("TVectorizable", true);

// ceil
PrimExpr ceil(PrimExpr x) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  using tir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImm(x.dtype(), std::ceil(fx->value));
  static auto op = Op::Get("tir.ceil");
  return tir::Call(x.dtype(), op, {x});
}

TIR_REGISTER_PURE_UNARY_OP("tir.ceil").set_attr<TVectorizable>("TVectorizable", true);

// round
PrimExpr round(PrimExpr x) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  using tir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImm(x.dtype(), std::nearbyint(fx->value));
  static auto op = Op::Get("tir.round");
  return tir::Call(x.dtype(), op, {x});
}

TIR_REGISTER_PURE_UNARY_OP("tir.round").set_attr<TVectorizable>("TVectorizable", true);

// nearbyint
PrimExpr nearbyint(PrimExpr x) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  using tir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) return FloatImm(x.dtype(), std::nearbyint(fx->value));
  static auto op = Op::Get("tir.nearbyint");
  return tir::Call(x.dtype(), op, {x});
}

TIR_REGISTER_PURE_UNARY_OP("tir.nearbyint");

// trunc
PrimExpr trunc(PrimExpr x) {
  if (x.dtype().is_int() || x.dtype().is_uint()) {
    return x;
  }
  using tir::FloatImmNode;
  const FloatImmNode* fx = x.as<FloatImmNode>();
  if (fx) {
    return FloatImm(x.dtype(), (fx->value < 0 ? std::ceil(fx->value) : std::floor(fx->value)));
  }
  static auto op = Op::Get("tir.trunc");
  return tir::Call(x.dtype(), op, {x});
}

TIR_REGISTER_PURE_UNARY_OP("tir.trunc").set_attr<TVectorizable>("TVectorizable", true);

// unary op registration.
TIR_REGISTER_PURE_UNARY_OP("tir.exp").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.exp2").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.exp10").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.erf");

TIR_REGISTER_PURE_UNARY_OP("tir.tanh").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.sigmoid");

TIR_REGISTER_PURE_UNARY_OP("tir.sqrt").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.rsqrt");

TIR_REGISTER_PURE_UNARY_OP("tir.log").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.log2").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.log1p");

TIR_REGISTER_PURE_UNARY_OP("tir.log10").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.tan").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.cos").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.cosh").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.sin").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.sinh").set_attr<TVectorizable>("TVectorizable", true);

TIR_REGISTER_PURE_UNARY_OP("tir.asin");

TIR_REGISTER_PURE_UNARY_OP("tir.acos");

TIR_REGISTER_PURE_UNARY_OP("tir.atan");

TIR_REGISTER_PURE_UNARY_OP("tir.acosh");

TIR_REGISTER_PURE_UNARY_OP("tir.asinh");

TIR_REGISTER_PURE_UNARY_OP("tir.atanh");

// binary intrinsics
TIR_REGISTER_PURE_BINARY_OP("tir.atan2");

TIR_REGISTER_PURE_BINARY_OP("tir.nextafter");

TIR_REGISTER_PURE_BINARY_OP("tir.hypot");

TIR_REGISTER_PURE_BINARY_OP("tir.copysign");

TIR_REGISTER_PURE_BINARY_OP("tir.ldexp");

// expose basic functions to node namespace
TVM_REGISTER_GLOBAL("node._const").set_body([](TVMArgs args, TVMRetValue* ret) {
  if (args[0].type_code() == kDLInt) {
    *ret = tir::make_const(args[1], args[0].operator int64_t());
  } else if (args[0].type_code() == kDLFloat) {
    *ret = tir::make_const(args[1], args[0].operator double());
  } else {
    LOG(FATAL) << "only accept int or float";
  }
});

TVM_REGISTER_GLOBAL("node.LargeUIntImm").set_body_typed(LargeUIntImm);

TVM_REGISTER_GLOBAL("tir.min_value").set_body_typed(min_value);

TVM_REGISTER_GLOBAL("tir.max_value").set_body_typed(max_value);

TVM_REGISTER_GLOBAL("tir.abs").set_body_typed(tvm::abs);

TVM_REGISTER_GLOBAL("tir.isnan").set_body_typed(tvm::isnan);

TVM_REGISTER_GLOBAL("tir.isfinite").set_body_typed(tvm::isfinite);

TVM_REGISTER_GLOBAL("tir.isinf").set_body_typed(tvm::isinf);

TVM_REGISTER_GLOBAL("tir.floor").set_body_typed(tvm::floor);

TVM_REGISTER_GLOBAL("tir.ceil").set_body_typed(tvm::ceil);

TVM_REGISTER_GLOBAL("tir.round").set_body_typed(tvm::round);

TVM_REGISTER_GLOBAL("tir.nearbyint").set_body_typed(tvm::nearbyint);

TVM_REGISTER_GLOBAL("tir.trunc").set_body_typed(tvm::trunc);

TVM_REGISTER_GLOBAL("tir._cast").set_body_typed(tvm::cast);

// operator overloading, smarter than make
#define REGISTER_MAKE_BINARY_OP(Node, Func)                                     \
  TVM_REGISTER_GLOBAL("tir." #Node).set_body_typed([](PrimExpr a, PrimExpr b) { \
    return (Func(a, b));                                                        \
  })

#define REGISTER_MAKE_BIT_OP(Node, Func)                                          \
  TVM_REGISTER_GLOBAL("tir." #Node).set_body([](TVMArgs args, TVMRetValue* ret) { \
    bool lhs_is_int = args[0].type_code() == kDLInt;                              \
    bool rhs_is_int = args[1].type_code() == kDLInt;                              \
    if (lhs_is_int) {                                                             \
      *ret = (Func(args[0].operator int(), args[1].operator PrimExpr()));         \
    } else if (rhs_is_int) {                                                      \
      *ret = (Func(args[0].operator PrimExpr(), args[1].operator int()));         \
    } else {                                                                      \
      *ret = (Func(args[0].operator PrimExpr(), args[1].operator PrimExpr()));    \
    }                                                                             \
  })

REGISTER_MAKE_BINARY_OP(_OpAdd, operator+);
REGISTER_MAKE_BINARY_OP(_OpSub, operator-);
REGISTER_MAKE_BINARY_OP(_OpMul, operator*);
REGISTER_MAKE_BINARY_OP(_OpDiv, div);
REGISTER_MAKE_BINARY_OP(_OpMod, truncmod);
REGISTER_MAKE_BINARY_OP(_OpIndexDiv, indexdiv);
REGISTER_MAKE_BINARY_OP(_OpIndexMod, indexmod);
REGISTER_MAKE_BINARY_OP(_OpFloorDiv, floordiv);
REGISTER_MAKE_BINARY_OP(_OpFloorMod, floormod);
REGISTER_MAKE_BINARY_OP(_OpTruncDiv, truncdiv);
REGISTER_MAKE_BINARY_OP(_OpTruncMod, truncmod);
REGISTER_MAKE_BINARY_OP(_OpPow, pow);
REGISTER_MAKE_BINARY_OP(_OpMin, min);
REGISTER_MAKE_BINARY_OP(_OpMax, max);
REGISTER_MAKE_BINARY_OP(_OpEQ, operator==);
REGISTER_MAKE_BINARY_OP(_OpNE, operator!=);
REGISTER_MAKE_BINARY_OP(_OpLT, operator<);   // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpLE, operator<=);  // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpGT, operator>);   // NOLINT(*)
REGISTER_MAKE_BINARY_OP(_OpGE, operator>=);
REGISTER_MAKE_BINARY_OP(_OpAnd, operator&&);
REGISTER_MAKE_BINARY_OP(_OpOr, operator||);
REGISTER_MAKE_BIT_OP(bitwise_and, operator&);
REGISTER_MAKE_BIT_OP(bitwise_or, operator|);
REGISTER_MAKE_BIT_OP(bitwise_xor, operator^);
REGISTER_MAKE_BIT_OP(left_shift, operator<<);  // NOLINT(*)
REGISTER_MAKE_BIT_OP(right_shift, operator>>);

TVM_REGISTER_GLOBAL("tir._OpIfThenElse")
    .set_body_typed([](PrimExpr cond, PrimExpr true_value, PrimExpr false_value) {
      return if_then_else(cond, true_value, false_value);
    });

}  // namespace tvm
