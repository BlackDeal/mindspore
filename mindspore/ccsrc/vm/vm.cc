/**
 * This is the C++ adaptation and derivative work of Myia (https://github.com/mila-iqia/myia/).
 *
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vm/vm.h"

#include <algorithm>

#include "vm/vmimpl.h"
#include "vm/backend.h"
#include "vm/transform.h"
#include "pipeline/parse/data_converter.h"

namespace mindspore {
namespace compile {

// Initialize StructPartial.
// Arguments:
//   fn_: Callable function.
//   args_: Sequence of function args.
StructPartial::StructPartial(int fn, const VectorRef& args) : fn_(fn), args_(args) {}

std::ostream& operator<<(std::ostream& os, const StructPartial& other) {
  os << "partial(" << other.fn_ << ", " << other.args_.ToString() << ")";
  return os;
}

bool operator==(const StructPartial& lhs, const StructPartial& rhs) {
  return (lhs.fn_ == rhs.fn_ && lhs.args_ == rhs.args_);
}

StructSimuSwitch::StructSimuSwitch(const BaseRef& fn, const BaseRef& value) : fn_(fn), value_(value) {}

std::ostream& operator<<(std::ostream& os, const StructSimuSwitch& other) {
  os << "SimulSwitch(" << other.fn_.ToString() << ", " << other.value_.ToString() << ")";
  return os;
}

bool operator==(const StructSimuSwitch& lhs, const StructSimuSwitch& rhs) {
  return (lhs.fn_ == rhs.fn_ && lhs.value_ == rhs.value_);
}

std::ostream& operator<<(std::ostream& os, const SwitchCondStatus& other) {
  os << "SwitchCondStatus(" << static_cast<int>(other) << ")";
  return os;
}

// Follow the specified instructions to create a VM.
// Arguments:
//   insts_: std::vector<std::map<std::string, VectorRef>>
//   insts_stack_: The value stack.
//   retp_: The call stack.
//   pc_: program counter (next instruction)
//   sp_: stack pointer (for the value stack)
FinalVM::FinalVM(const InstSet& insts, const BackendPtr& backend) : insts_(insts), pc_(0), sp_(0), backend_(backend) {
  MS_LOG(DEBUG) << "InstSet size:" << insts_.size();
  insts_stack_.emplace_back(BaseRef());
  retp_.push(-1);
}

void FinalVM::Push(const BaseRef& v) {
  MS_LOG(DEBUG) << "Push " << v.ToString() << " sp_:" << sp_;
  insts_stack_[IntToSize(sp_++)] = v;
}

void FinalVM::Pop(int n) {
  if (n > sp_) {
    MS_LOG(EXCEPTION) << "Invalid value of n " << n << ", it should be not more than " << sp_ - 1;
  }
  for (int i = 0; i < n; i++) {
    insts_stack_[IntToSize(sp_ - i - 1)] = BaseRef();
  }
  sp_ -= n;
}

void FinalVM::MoveStack(int nitems, int height) {
  if (nitems > height || height > sp_) {
    MS_LOG(EXCEPTION) << "MoveStack arg error: nitems=" << nitems << " height=" << height;
  }
  int n = height - nitems;
  int src = sp_ - height;
  int dst = sp_ - nitems;
  for (int i = 0; i < nitems; i++) {
    insts_stack_[IntToSize(src + i)] = insts_stack_[IntToSize(dst + i)];
  }
  Pop(n);
}

BaseRef FinalVM::Ref(int i) {
  MS_LOG(DEBUG) << "Ref i:" << i << " sp_:" << sp_;
  size_t sp_next = IntToSize(sp_ + i);
  if (sp_next < insts_stack_.size()) {
    if (utils::isa<PyObjectRef>(insts_stack_[sp_next])) {
      py::object value = utils::cast<PyObjectRef>(insts_stack_[sp_next]).object_;
      MS_LOG(DEBUG) << "VM ref python:" << py::str(value);
      return parse::data_converter::PyDataToValue(value);
    }
    MS_LOG(DEBUG) << "Ref not python :" << insts_stack_[sp_next].ToString();
    return insts_stack_[sp_next];
  }

  MS_LOG(EXCEPTION) << "IndexError: index(" << sp_next << ") out of range [0, " << insts_stack_.size() << ").";
}

void FinalVM::Pushp() { retp_.push(pc_); }

void FinalVM::Popp() {
  if (retp_.empty()) {
    MS_LOG(EXCEPTION) << "Stack retp_ is empty";
  }
  pc_ = retp_.top();
  MS_LOG(DEBUG) << "Pop pc:" << pc_ << ", sp:" << sp_;
  retp_.pop();
}

void FinalVM::Pushsp() { retsp_.push(sp_); }

void FinalVM::Popsp() {
  int sp = retsp_.top();
  MS_LOG(DEBUG) << "Current sp:" << sp_ << ", before sp:" << sp << ", " << sp_ - sp;
  if (sp_ >= sp) {
    Pop(sp_ - sp + 1);
    retsp_.pop();
  } else {
    MS_LOG(EXCEPTION) << "Stack point sp_:" << sp << " must biger than sp:" << sp_;
  }
}

void FinalVM::DoJmp(const BaseRef& jmp_orig) {
  MS_LOG(DEBUG) << "Start";

  BaseRef jmp = jmp_orig;
  if (backend_->simu_flag()) {
    if (utils::isa<StructSimuSwitch>(jmp)) {  // need to inherit from Base
      MS_LOG(DEBUG) << "Start jump StructSwitch";
      auto simu_value = utils::cast<std::shared_ptr<StructSimuSwitch>>(jmp);
      jmp = simu_value->fn_;
      backend_->set_curr_switch(simu_value->value_);
    }
  }

  if (utils::isa<StructPartial>(jmp)) {  // need to inherit from Base
    MS_LOG(DEBUG) << "Start jump StructPartial";
    auto new_jmp = utils::cast<std::shared_ptr<StructPartial>>(jmp);
    auto args = new_jmp->args_;
    InstPadStack(VectorRef(std::vector<BaseRef>{static_cast<int>(args.size())}));
    auto iter = args.rbegin();
    for (; iter != args.rend(); ++iter) {
      Push(*iter);
    }
    pc_ = new_jmp->fn_;
    return;
  }

  if (!utils::isa<int>(jmp)) {
    MS_LOG(EXCEPTION) << "Jmp inst should be a int";
  }
  pc_ = utils::cast<int>(jmp);
  MS_LOG(DEBUG) << "End do jump pc_:" << pc_;
}

BaseRef FinalVM::Eval(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start: " << args.size();
  insts_stack_.clear();
  insts_stack_.resize(args.size());
  std::stack<int>().swap(retp_);
  retp_.push(-1);
  pc_ = 0;
  sp_ = 0;

  auto riter = args.rbegin();
  for (; riter != args.rend(); ++riter) {
    if (utils::isa<PyObjectRef>(*riter)) {
      PyObjectRef py_ref = utils::cast<PyObjectRef>(*riter);
      py::object value = py_ref.object_;
      if (py::isinstance<py::bool_>(value)) {
        auto a = py::cast<bool>(value);
        Push(static_cast<int>(a));
        continue;
      }
    }
    Push(*riter);
  }

  while (pc_ >= 0) {
    auto inst = insts_[IntToSize(pc_)];
    MS_LOG(DEBUG) << "Loop " << insts_.size() << ", pc:" << pc_ << ", inst:" << inst_str[inst.first];
    ++pc_;
    auto iter = inst_function_map.find(inst.first);
    if (iter != inst_function_map.end()) {
      iter->second(inst.second);
    } else {
      MS_LOG(EXCEPTION) << "Unknown instruction {" << inst_str[inst.first] << "}";
    }
  }

  MS_LOG(DEBUG) << "End";
  return insts_stack_[0];
}

void FinalVM::InstCall(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 1;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameter, while the input size is "
                  << args.size() << ".";
    return;
  }

  int jmp = utils::cast<int>(args[0]);
  MS_LOG(DEBUG) << "Call pushp:" << pc_ << ", jmp:" << jmp << ", sp:" << sp_;
  Pushp();
  DoJmp(Ref(jmp));
  MS_LOG(DEBUG) << "Instcall end sp :" << sp_;
}

void FinalVM::InstTailCall(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 3;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameters, while the input size is "
                  << args.size() << ".";
    return;
  }

  int jmp = utils::cast<int>(args[0]);
  int height = utils::cast<int>(args[1]);
  int nargs = utils::cast<int>(args[2]);

  auto new_jmp = Ref(jmp);

  if (backend_->simu_flag()) {
    if (backend_->GetSimuCondFlag(BaseRef()) == 2) {
      MS_LOG(DEBUG) << "invoke while call tail first";
      Pop(height);
      Push(1);
      Popp();
      return;
    }
  }
  MoveStack(nargs, height);
  MS_LOG(DEBUG) << "TailCall pushp:" << pc_ << ", jmp:" << jmp;
  DoJmp(new_jmp);
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstSwitchReturn(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  if (args.size() != 1) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires one parameter, while the input size is " << args.size() << ".";
    return;
  }
  Pop(1);
  Popsp();
}

void FinalVM::InstReturn(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 2;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameters, while the input size is "
                  << args.size() << ".";
    return;
  }

  int rpos = utils::cast<int>(args[0]);
  int height = utils::cast<int>(args[1]);

  auto rv = Ref(rpos);
  if (backend_->simu_flag() && backend_->is_switch_call()) {
    backend_->SetSwitchGraph();
  }

  Pop(height);
  Push(rv);
  Popp();
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstPartial(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 1;
  if (args.size() < args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " or more parameters, while the input size is "
                  << args.size() << ".";
    return;
  }

  int fn_ = utils::cast<int>(args[0]);
  auto fn = utils::cast<int>(Ref(fn_));
  MS_LOG(DEBUG) << "Partial argssize:" << args.size();
  std::vector<BaseRef> outs(args.size() - 1);

  (void)std::transform(args.begin() + 1, args.end(), outs.begin(),
                       [&, this](const BaseRef& a) { return Ref(utils::cast<int>(a)); });
  Push(std::make_shared<StructPartial>(fn, VectorRef(outs)));
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstSimuSwitch(const VectorRef& args) {
  const size_t args_size = 4;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameters, while the input size is "
                  << args.size() << ".";
    return;
  }
  bool cond = utils::cast<bool>(args[0]);
  int cond_node = utils::cast<int>(args[1]);
  int vtrue = utils::cast<int>(args[2]);
  int vfalse = utils::cast<int>(args[3]);

  MS_LOG(DEBUG) << "Simu switch cond:" << cond;
  BaseRef c = Ref(cond_node);
  bool bool_value = cond;
  SwitchCondStatus cond_stat = backend_->SetSimuCond(c, bool_value);

  int cond_flag = backend_->GetSimuCondFlag(c);
  MS_LOG(DEBUG) << "Simu switch cond:" << cond << ", " << cond_flag << ", " << c.cast<AnfNodePtr>()->DebugString();
  if (cond_flag == 2) {
    Popp();
    Popp();
    backend_->SetSimuCondFlag(c, 0);
    return;
  }

  if (cond_stat == kCondAlreadyRun) {
    MS_LOG(DEBUG) << "switch alreay run bool while true jmp";
    if (cond_flag == 0) {
      MS_LOG(DEBUG) << "switch second run bool while true jmp";
      backend_->SetSwitchActive(c, true);
      Push(std::make_shared<StructSimuSwitch>(Ref(vtrue), c));
      Pushsp();
      backend_->SetSimuCondFlag(c, 1);
      return;
    } else if (cond_flag == 1) {
      MS_LOG(DEBUG) << "switch first run bool while if jmp";
      Push(std::make_shared<StructSimuSwitch>(Ref(vfalse), c));
      (void)backend_->SetSimuCond(c, false);
      backend_->SetSimuCondFlag(c, 2);
      return;
    } else {
      MS_LOG(EXCEPTION) << "error cond not find";
      return;
    }
  }
  if (bool_value) {
    Push(std::make_shared<StructSimuSwitch>(Ref(vtrue), c));
    Pushsp();
  } else {
    Push(std::make_shared<StructSimuSwitch>(Ref(vfalse), c));
  }
}

void FinalVM::InstRealSwitch(const VectorRef& args) {
  const size_t args_size = 3;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameters, while the input size is "
                  << args.size() << ".";
    return;
  }

  int cond = utils::cast<int>(args[0]);
  int vtrue = utils::cast<int>(args[1]);
  int vfalse = utils::cast<int>(args[2]);

  BaseRef c = Ref(cond);
  MS_LOG(DEBUG) << "" << vtrue << " false:" << vfalse << " InstSwitch: " << c.ToString();
  bool bool_value = false;
  if (backend_->GetCond(c, &bool_value)) {
    MS_LOG(DEBUG) << "Cond:" << bool_value;
    if (bool_value) {
      Push(Ref(vtrue));
    } else {
      Push(Ref(vfalse));
    }
  } else {
    MS_LOG(EXCEPTION) << "Not supported type to be casted to bool";
  }
}

void FinalVM::InstSwitch(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  if (backend_->is_multi_graph_sink()) {
    InstSimuSwitch(args);
  } else {
    InstRealSwitch(args);
  }
}

void FinalVM::InstTuple(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  VectorRef tuple;
  auto iter = args.begin();
  for (; iter != args.end(); ++iter) {
    auto a = utils::cast<int>(*iter);
    tuple.push_back(Ref(a));
  }
  Push(tuple);
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstPush(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 1;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameter, while the input size is "
                  << args.size() << ".";
    return;
  }

  auto v = args[0];
  Push(v);
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstInput(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 1;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameter, while the input size is "
                  << args.size() << ".";
    return;
  }

  int rpos = utils::cast<int>(args[0]);
  Push(Ref(rpos));
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstPadStack(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start";
  const size_t args_size = 1;
  if (args.size() != args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " parameter, while the input size is "
                  << args.size() << ".";
    return;
  }

  int sz = utils::cast<int>(args[0]);
  MS_LOG(DEBUG) << "" << insts_stack_.size() << " need padstack " << sz << " sp_ " << sp_;
  size_t stack_size = insts_stack_.size();
  int need = sz - (static_cast<int>(stack_size) - sp_);
  if (need > 0) {
    MS_LOG(DEBUG) << "InstPadStack resize: size:" << insts_stack_.size() << " need pad:" << need;
    insts_stack_.resize(stack_size + IntToSize(need));
  }
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstExternal(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start:" << args.size();

  if (args.empty()) {
    MS_LOG(EXCEPTION) << "Args is empty!";
  }

  VectorRef tuple;
  RunFunctionRef run_ref = utils::cast<RunFunctionRef>(args[0]);
  compile::RunFuncPtr fn = run_ref.func_;
  if (backend_->simu_flag()) {
    MS_LOG(DEBUG) << "Simu run";
    if (args.size() == 1) {
      MS_LOG(EXCEPTION) << "The number of args should be greater than 1, but got 1";
    }
    auto simu_run_ref = utils::cast<RunFunctionRef>(args[1]);
    fn = simu_run_ref.func_;
  }
  for (size_t i = 2; i < args.size(); ++i) {
    auto index = utils::cast<int>(args[i]);
    tuple.push_back(Ref(index));
  }

  if (!fn) {
    MS_LOG(EXCEPTION) << "Function not callable";
  }

  auto outs = (*fn)(tuple);
  MS_LOG(DEBUG) << "'fn' out size:" << outs.size();
  for (auto& o : outs) {
    MS_LOG(DEBUG) << "InstExternal value:" << o.ToString();
    Push(o);
  }
  MS_LOG(DEBUG) << "End";
}

void FinalVM::InstPushPrim(const VectorRef& args) {
  MS_LOG(DEBUG) << "Start: " << args.size();
  const size_t args_size = 2;
  if (args.size() < args_size) {
    MS_LOG(ERROR) << "" << __FUNCTION__ << " requires " << args_size << " or more parameters, while the input size is "
                  << args.size() << ".";
    return;
  }

  VectorRef tuple;
  auto prim = utils::cast<PrimitivePtr>(args[0]);
  for (size_t i = 1; i < args.size(); ++i) {
    auto index = utils::cast<int>(args[1]);
    tuple.push_back(Ref(index));
  }

  auto outs = RunOperation(prim, tuple);
  Push(outs);

  MS_LOG(DEBUG) << "End";
}

}  // namespace compile
}  // namespace mindspore
