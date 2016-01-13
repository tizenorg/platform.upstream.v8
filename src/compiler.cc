// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler.h"

#include <algorithm>

#include "src/ast-numbering.h"
#include "src/bootstrapper.h"
#include "src/codegen.h"
#include "src/compilation-cache.h"
#include "src/compiler/pipeline.h"
#include "src/debug/debug.h"
#include "src/debug/liveedit.h"
#include "src/deoptimizer.h"
#include "src/full-codegen/full-codegen.h"
#include "src/gdb-jit.h"
#include "src/hydrogen.h"
#include "src/interpreter/interpreter.h"
#include "src/isolate-inl.h"
#include "src/lithium.h"
#include "src/log-inl.h"
#include "src/messages.h"
#include "src/parser.h"
#include "src/prettyprinter.h"
#include "src/profiler/cpu-profiler.h"
#include "src/rewriter.h"
#include "src/runtime-profiler.h"
#include "src/scanner-character-streams.h"
#include "src/scopeinfo.h"
#include "src/scopes.h"
#include "src/snapshot/serialize.h"
#include "src/typing.h"
#include "src/vm-state-inl.h"

namespace v8 {
namespace internal {

std::ostream& operator<<(std::ostream& os, const SourcePosition& p) {
  if (p.IsUnknown()) {
    return os << "<?>";
  } else if (FLAG_hydrogen_track_positions) {
    return os << "<" << p.inlining_id() << ":" << p.position() << ">";
  } else {
    return os << "<0:" << p.raw() << ">";
  }
}


#define PARSE_INFO_GETTER(type, name)  \
  type CompilationInfo::name() const { \
    CHECK(parse_info());               \
    return parse_info()->name();       \
  }


#define PARSE_INFO_GETTER_WITH_DEFAULT(type, name, def) \
  type CompilationInfo::name() const {                  \
    return parse_info() ? parse_info()->name() : def;   \
  }


PARSE_INFO_GETTER(Handle<Script>, script)
PARSE_INFO_GETTER(bool, is_eval)
PARSE_INFO_GETTER(bool, is_native)
PARSE_INFO_GETTER(bool, is_module)
PARSE_INFO_GETTER(FunctionLiteral*, literal)
PARSE_INFO_GETTER_WITH_DEFAULT(LanguageMode, language_mode, STRICT)
PARSE_INFO_GETTER_WITH_DEFAULT(Handle<JSFunction>, closure,
                               Handle<JSFunction>::null())
PARSE_INFO_GETTER_WITH_DEFAULT(Scope*, scope, nullptr)
PARSE_INFO_GETTER(Handle<Context>, context)
PARSE_INFO_GETTER(Handle<SharedFunctionInfo>, shared_info)

#undef PARSE_INFO_GETTER
#undef PARSE_INFO_GETTER_WITH_DEFAULT


// Exactly like a CompilationInfo, except being allocated via {new} and it also
// creates and enters a Zone on construction and deallocates it on destruction.
class CompilationInfoWithZone : public CompilationInfo {
 public:
  explicit CompilationInfoWithZone(Handle<JSFunction> function)
      : CompilationInfo(new ParseInfo(&zone_, function)) {}

  // Virtual destructor because a CompilationInfoWithZone has to exit the
  // zone scope and get rid of dependent maps even when the destructor is
  // called when cast as a CompilationInfo.
  virtual ~CompilationInfoWithZone() {
    DisableFutureOptimization();
    dependencies()->Rollback();
    delete parse_info_;
    parse_info_ = nullptr;
  }

 private:
  Zone zone_;
};


bool CompilationInfo::has_shared_info() const {
  return parse_info_ && !parse_info_->shared_info().is_null();
}


bool CompilationInfo::has_context() const {
  return parse_info_ && !parse_info_->context().is_null();
}


bool CompilationInfo::has_literal() const {
  return parse_info_ && parse_info_->literal() != nullptr;
}


bool CompilationInfo::has_scope() const {
  return parse_info_ && parse_info_->scope() != nullptr;
}


CompilationInfo::CompilationInfo(ParseInfo* parse_info)
    : CompilationInfo(parse_info, nullptr, nullptr, BASE, parse_info->isolate(),
                      parse_info->zone()) {
  // Compiling for the snapshot typically results in different code than
  // compiling later on. This means that code recompiled with deoptimization
  // support won't be "equivalent" (as defined by SharedFunctionInfo::
  // EnableDeoptimizationSupport), so it will replace the old code and all
  // its type feedback. To avoid this, always compile functions in the snapshot
  // with deoptimization support.
  if (isolate_->serializer_enabled()) EnableDeoptimizationSupport();

  if (FLAG_function_context_specialization) MarkAsFunctionContextSpecializing();
  if (FLAG_turbo_inlining) MarkAsInliningEnabled();
  if (FLAG_turbo_source_positions) MarkAsSourcePositionsEnabled();
  if (FLAG_turbo_splitting) MarkAsSplittingEnabled();
  if (FLAG_turbo_types) MarkAsTypingEnabled();

  if (has_shared_info()) {
    if (shared_info()->is_compiled()) {
      // We should initialize the CompilationInfo feedback vector from the
      // passed in shared info, rather than creating a new one.
      feedback_vector_ = Handle<TypeFeedbackVector>(
          shared_info()->feedback_vector(), parse_info->isolate());
    }
    if (shared_info()->never_compiled()) MarkAsFirstCompile();
  }
}


CompilationInfo::CompilationInfo(CodeStub* stub, Isolate* isolate, Zone* zone)
    : CompilationInfo(nullptr, stub, CodeStub::MajorName(stub->MajorKey()),
                      STUB, isolate, zone) {}

CompilationInfo::CompilationInfo(const char* debug_name, Isolate* isolate,
                                 Zone* zone)
    : CompilationInfo(nullptr, nullptr, debug_name, STUB, isolate, zone) {
  set_output_code_kind(Code::STUB);
}

CompilationInfo::CompilationInfo(ParseInfo* parse_info, CodeStub* code_stub,
                                 const char* debug_name, Mode mode,
                                 Isolate* isolate, Zone* zone)
    : parse_info_(parse_info),
      isolate_(isolate),
      flags_(0),
      code_stub_(code_stub),
      mode_(mode),
      osr_ast_id_(BailoutId::None()),
      zone_(zone),
      deferred_handles_(nullptr),
      dependencies_(isolate, zone),
      bailout_reason_(kNoReason),
      prologue_offset_(Code::kPrologueOffsetNotSet),
      no_frame_ranges_(isolate->cpu_profiler()->is_profiling()
                           ? new List<OffsetRange>(2)
                           : nullptr),
      track_positions_(FLAG_hydrogen_track_positions ||
                       isolate->cpu_profiler()->is_profiling()),
      opt_count_(has_shared_info() ? shared_info()->opt_count() : 0),
      parameter_count_(0),
      optimization_id_(-1),
      osr_expr_stack_height_(0),
      function_type_(nullptr),
      debug_name_(debug_name) {
  // Parameter count is number of stack parameters.
  if (code_stub_ != NULL) {
    CodeStubDescriptor descriptor(code_stub_);
    parameter_count_ = descriptor.GetStackParameterCount();
    if (descriptor.function_mode() == NOT_JS_FUNCTION_STUB_MODE) {
      parameter_count_--;
    }
    set_output_code_kind(code_stub->GetCodeKind());
  } else {
    set_output_code_kind(Code::FUNCTION);
  }
}


CompilationInfo::~CompilationInfo() {
  DisableFutureOptimization();
  delete deferred_handles_;
  delete no_frame_ranges_;
#ifdef DEBUG
  // Check that no dependent maps have been added or added dependent maps have
  // been rolled back or committed.
  DCHECK(dependencies()->IsEmpty());
#endif  // DEBUG
}


void CompilationInfo::SetStub(CodeStub* code_stub) {
  SetMode(STUB);
  code_stub_ = code_stub;
  debug_name_ = CodeStub::MajorName(code_stub->MajorKey());
  set_output_code_kind(code_stub->GetCodeKind());
}


int CompilationInfo::num_parameters() const {
  return has_scope() ? scope()->num_parameters() : parameter_count_;
}


int CompilationInfo::num_parameters_including_this() const {
  return num_parameters() + (is_this_defined() ? 1 : 0);
}


bool CompilationInfo::is_this_defined() const { return !IsStub(); }


int CompilationInfo::num_heap_slots() const {
  return has_scope() ? scope()->num_heap_slots() : 0;
}


// Primitive functions are unlikely to be picked up by the stack-walking
// profiler, so they trigger their own optimization when they're called
// for the SharedFunctionInfo::kCallsUntilPrimitiveOptimization-th time.
bool CompilationInfo::ShouldSelfOptimize() {
  return FLAG_crankshaft &&
         !(literal()->flags() & AstProperties::kDontSelfOptimize) &&
         !literal()->dont_optimize() &&
         literal()->scope()->AllowsLazyCompilation() &&
         (!has_shared_info() || !shared_info()->optimization_disabled());
}


void CompilationInfo::EnsureFeedbackVector() {
  if (feedback_vector_.is_null()) {
    feedback_vector_ = isolate()->factory()->NewTypeFeedbackVector(
        literal()->feedback_vector_spec());
  }

  // It's very important that recompiles do not alter the structure of the
  // type feedback vector.
  CHECK(!feedback_vector_->SpecDiffersFrom(literal()->feedback_vector_spec()));
}


bool CompilationInfo::has_simple_parameters() {
  return scope()->has_simple_parameters();
}


int CompilationInfo::TraceInlinedFunction(Handle<SharedFunctionInfo> shared,
                                          SourcePosition position,
                                          int parent_id) {
  DCHECK(track_positions_);

  int inline_id = static_cast<int>(inlined_function_infos_.size());
  InlinedFunctionInfo info(parent_id, position, UnboundScript::kNoScriptId,
      shared->start_position());
  if (!shared->script()->IsUndefined()) {
    Handle<Script> script(Script::cast(shared->script()));
    info.script_id = script->id();

    if (FLAG_hydrogen_track_positions && !script->source()->IsUndefined()) {
      CodeTracer::Scope tracing_scope(isolate()->GetCodeTracer());
      OFStream os(tracing_scope.file());
      os << "--- FUNCTION SOURCE (" << shared->DebugName()->ToCString().get()
         << ") id{" << optimization_id() << "," << inline_id << "} ---\n";
      {
        DisallowHeapAllocation no_allocation;
        int start = shared->start_position();
        int len = shared->end_position() - start;
        String::SubStringRange source(String::cast(script->source()), start,
                                      len);
        for (const auto& c : source) {
          os << AsReversiblyEscapedUC16(c);
        }
      }

      os << "\n--- END ---\n";
    }
  }

  inlined_function_infos_.push_back(info);

  if (FLAG_hydrogen_track_positions && inline_id != 0) {
    CodeTracer::Scope tracing_scope(isolate()->GetCodeTracer());
    OFStream os(tracing_scope.file());
    os << "INLINE (" << shared->DebugName()->ToCString().get() << ") id{"
       << optimization_id() << "," << inline_id << "} AS " << inline_id
       << " AT " << position << std::endl;
  }

  return inline_id;
}


void CompilationInfo::LogDeoptCallPosition(int pc_offset, int inlining_id) {
  if (!track_positions_ || IsStub()) return;
  DCHECK_LT(static_cast<size_t>(inlining_id), inlined_function_infos_.size());
  inlined_function_infos_.at(inlining_id).deopt_pc_offsets.push_back(pc_offset);
}


base::SmartArrayPointer<char> CompilationInfo::GetDebugName() const {
  if (parse_info()) {
    AllowHandleDereference allow_deref;
    return parse_info()->literal()->debug_name()->ToCString();
  }
  const char* str = debug_name_ ? debug_name_ : "unknown";
  size_t len = strlen(str) + 1;
  base::SmartArrayPointer<char> name(new char[len]);
  memcpy(name.get(), str, len);
  return name;
}


bool CompilationInfo::MustReplaceUndefinedReceiverWithGlobalProxy() {
  return is_sloppy(language_mode()) && !is_native() &&
         scope()->has_this_declaration() && scope()->receiver()->is_used();
}


class HOptimizedGraphBuilderWithPositions: public HOptimizedGraphBuilder {
 public:
  explicit HOptimizedGraphBuilderWithPositions(CompilationInfo* info)
      : HOptimizedGraphBuilder(info) {
  }

#define DEF_VISIT(type)                                      \
  void Visit##type(type* node) override {                    \
    SourcePosition old_position = SourcePosition::Unknown(); \
    if (node->position() != RelocInfo::kNoPosition) {        \
      old_position = source_position();                      \
      SetSourcePosition(node->position());                   \
    }                                                        \
    HOptimizedGraphBuilder::Visit##type(node);               \
    if (!old_position.IsUnknown()) {                         \
      set_source_position(old_position);                     \
    }                                                        \
  }
  EXPRESSION_NODE_LIST(DEF_VISIT)
#undef DEF_VISIT

#define DEF_VISIT(type)                                      \
  void Visit##type(type* node) override {                    \
    SourcePosition old_position = SourcePosition::Unknown(); \
    if (node->position() != RelocInfo::kNoPosition) {        \
      old_position = source_position();                      \
      SetSourcePosition(node->position());                   \
    }                                                        \
    HOptimizedGraphBuilder::Visit##type(node);               \
    if (!old_position.IsUnknown()) {                         \
      set_source_position(old_position);                     \
    }                                                        \
  }
  STATEMENT_NODE_LIST(DEF_VISIT)
#undef DEF_VISIT

#define DEF_VISIT(type)                        \
  void Visit##type(type* node) override {      \
    HOptimizedGraphBuilder::Visit##type(node); \
  }
  DECLARATION_NODE_LIST(DEF_VISIT)
#undef DEF_VISIT
};


OptimizedCompileJob::Status OptimizedCompileJob::CreateGraph() {
  DCHECK(info()->IsOptimizing());

  // Do not use Crankshaft/TurboFan if we need to be able to set break points.
  if (info()->shared_info()->HasDebugInfo()) {
    return AbortOptimization(kFunctionBeingDebugged);
  }

  // Limit the number of times we try to optimize functions.
  const int kMaxOptCount =
      FLAG_deopt_every_n_times == 0 ? FLAG_max_opt_count : 1000;
  if (info()->opt_count() > kMaxOptCount) {
    return AbortOptimization(kOptimizedTooManyTimes);
  }

  // Check the whitelist for Crankshaft.
  if (!info()->closure()->PassesFilter(FLAG_hydrogen_filter)) {
    return AbortOptimization(kHydrogenFilter);
  }

  // Optimization requires a version of fullcode with deoptimization support.
  // Recompile the unoptimized version of the code if the current version
  // doesn't have deoptimization support already.
  // Otherwise, if we are gathering compilation time and space statistics
  // for hydrogen, gather baseline statistics for a fullcode compilation.
  bool should_recompile = !info()->shared_info()->has_deoptimization_support();
  if (should_recompile || FLAG_hydrogen_stats) {
    base::ElapsedTimer timer;
    if (FLAG_hydrogen_stats) {
      timer.Start();
    }
    if (!Compiler::EnsureDeoptimizationSupport(info())) {
      return SetLastStatus(FAILED);
    }
    if (FLAG_hydrogen_stats) {
      isolate()->GetHStatistics()->IncrementFullCodeGen(timer.Elapsed());
    }
  }

  DCHECK(info()->shared_info()->has_deoptimization_support());
  DCHECK(!info()->is_first_compile());

  // Check the enabling conditions for TurboFan.
  bool dont_crankshaft = info()->shared_info()->dont_crankshaft();
  if (((FLAG_turbo_asm && info()->shared_info()->asm_function()) ||
       (dont_crankshaft && strcmp(FLAG_turbo_filter, "~~") == 0) ||
       info()->closure()->PassesFilter(FLAG_turbo_filter)) &&
      (FLAG_turbo_osr || !info()->is_osr())) {
    // Use TurboFan for the compilation.
    if (FLAG_trace_opt) {
      OFStream os(stdout);
      os << "[compiling method " << Brief(*info()->closure())
         << " using TurboFan";
      if (info()->is_osr()) os << " OSR";
      os << "]" << std::endl;
    }

    if (info()->shared_info()->asm_function()) {
      if (info()->osr_frame()) info()->MarkAsFrameSpecializing();
      info()->MarkAsFunctionContextSpecializing();
    } else if (FLAG_turbo_type_feedback) {
      info()->MarkAsTypeFeedbackEnabled();
      info()->EnsureFeedbackVector();
    }
    if (!info()->shared_info()->asm_function() ||
        FLAG_turbo_asm_deoptimization) {
      info()->MarkAsDeoptimizationEnabled();
    }

    Timer t(this, &time_taken_to_create_graph_);
    compiler::Pipeline pipeline(info());
    pipeline.GenerateCode();
    if (!info()->code().is_null()) {
      return SetLastStatus(SUCCEEDED);
    }
  }

  if (!isolate()->use_crankshaft() || dont_crankshaft) {
    // Crankshaft is entirely disabled.
    return SetLastStatus(FAILED);
  }

  Scope* scope = info()->scope();
  if (LUnallocated::TooManyParameters(scope->num_parameters())) {
    // Crankshaft would require too many Lithium operands.
    return AbortOptimization(kTooManyParameters);
  }

  if (info()->is_osr() &&
      LUnallocated::TooManyParametersOrStackSlots(scope->num_parameters(),
                                                  scope->num_stack_slots())) {
    // Crankshaft would require too many Lithium operands.
    return AbortOptimization(kTooManyParametersLocals);
  }

  if (scope->HasIllegalRedeclaration()) {
    // Crankshaft cannot handle illegal redeclarations.
    return AbortOptimization(kFunctionWithIllegalRedeclaration);
  }

  if (FLAG_trace_opt) {
    OFStream os(stdout);
    os << "[compiling method " << Brief(*info()->closure())
       << " using Crankshaft";
    if (info()->is_osr()) os << " OSR";
    os << "]" << std::endl;
  }

  if (FLAG_trace_hydrogen) {
    isolate()->GetHTracer()->TraceCompilation(info());
  }

  // Type-check the function.
  AstTyper(info()->isolate(), info()->zone(), info()->closure(),
           info()->scope(), info()->osr_ast_id(), info()->literal())
      .Run();

  // Optimization could have been disabled by the parser. Note that this check
  // is only needed because the Hydrogen graph builder is missing some bailouts.
  if (info()->shared_info()->optimization_disabled()) {
    return AbortOptimization(
        info()->shared_info()->disable_optimization_reason());
  }

  graph_builder_ = (info()->is_tracking_positions() || FLAG_trace_ic)
                       ? new (info()->zone())
                             HOptimizedGraphBuilderWithPositions(info())
                       : new (info()->zone()) HOptimizedGraphBuilder(info());

  Timer t(this, &time_taken_to_create_graph_);
  graph_ = graph_builder_->CreateGraph();

  if (isolate()->has_pending_exception()) {
    return SetLastStatus(FAILED);
  }

  if (graph_ == NULL) return SetLastStatus(BAILED_OUT);

  if (info()->dependencies()->HasAborted()) {
    // Dependency has changed during graph creation. Let's try again later.
    return RetryOptimization(kBailedOutDueToDependencyChange);
  }

  return SetLastStatus(SUCCEEDED);
}


OptimizedCompileJob::Status OptimizedCompileJob::OptimizeGraph() {
  DisallowHeapAllocation no_allocation;
  DisallowHandleAllocation no_handles;
  DisallowHandleDereference no_deref;
  DisallowCodeDependencyChange no_dependency_change;

  DCHECK(last_status() == SUCCEEDED);
  // TODO(turbofan): Currently everything is done in the first phase.
  if (!info()->code().is_null()) {
    return last_status();
  }

  Timer t(this, &time_taken_to_optimize_);
  DCHECK(graph_ != NULL);
  BailoutReason bailout_reason = kNoReason;

  if (graph_->Optimize(&bailout_reason)) {
    chunk_ = LChunk::NewChunk(graph_);
    if (chunk_ != NULL) return SetLastStatus(SUCCEEDED);
  } else if (bailout_reason != kNoReason) {
    graph_builder_->Bailout(bailout_reason);
  }

  return SetLastStatus(BAILED_OUT);
}


OptimizedCompileJob::Status OptimizedCompileJob::GenerateCode() {
  DCHECK(last_status() == SUCCEEDED);
  // TODO(turbofan): Currently everything is done in the first phase.
  if (!info()->code().is_null()) {
    info()->dependencies()->Commit(info()->code());
    if (info()->is_deoptimization_enabled()) {
      info()->parse_info()->context()->native_context()->AddOptimizedCode(
          *info()->code());
    }
    RecordOptimizationStats();
    return last_status();
  }

  DCHECK(!info()->dependencies()->HasAborted());
  DisallowCodeDependencyChange no_dependency_change;
  DisallowJavascriptExecution no_js(isolate());
  {  // Scope for timer.
    Timer timer(this, &time_taken_to_codegen_);
    DCHECK(chunk_ != NULL);
    DCHECK(graph_ != NULL);
    // Deferred handles reference objects that were accessible during
    // graph creation.  To make sure that we don't encounter inconsistencies
    // between graph creation and code generation, we disallow accessing
    // objects through deferred handles during the latter, with exceptions.
    DisallowDeferredHandleDereference no_deferred_handle_deref;
    Handle<Code> optimized_code = chunk_->Codegen();
    if (optimized_code.is_null()) {
      if (info()->bailout_reason() == kNoReason) {
        return AbortOptimization(kCodeGenerationFailed);
      }
      return SetLastStatus(BAILED_OUT);
    }
    info()->SetCode(optimized_code);
  }
  RecordOptimizationStats();
  // Add to the weak list of optimized code objects.
  info()->context()->native_context()->AddOptimizedCode(*info()->code());
  return SetLastStatus(SUCCEEDED);
}


void OptimizedCompileJob::RecordOptimizationStats() {
  Handle<JSFunction> function = info()->closure();
  if (!function->IsOptimized()) {
    // Concurrent recompilation and OSR may race.  Increment only once.
    int opt_count = function->shared()->opt_count();
    function->shared()->set_opt_count(opt_count + 1);
  }
  double ms_creategraph = time_taken_to_create_graph_.InMillisecondsF();
  double ms_optimize = time_taken_to_optimize_.InMillisecondsF();
  double ms_codegen = time_taken_to_codegen_.InMillisecondsF();
  if (FLAG_trace_opt) {
    PrintF("[optimizing ");
    function->ShortPrint();
    PrintF(" - took %0.3f, %0.3f, %0.3f ms]\n", ms_creategraph, ms_optimize,
           ms_codegen);
  }
  if (FLAG_trace_opt_stats) {
    static double compilation_time = 0.0;
    static int compiled_functions = 0;
    static int code_size = 0;

    compilation_time += (ms_creategraph + ms_optimize + ms_codegen);
    compiled_functions++;
    code_size += function->shared()->SourceSize();
    PrintF("Compiled: %d functions with %d byte source size in %fms.\n",
           compiled_functions,
           code_size,
           compilation_time);
  }
  if (FLAG_hydrogen_stats) {
    isolate()->GetHStatistics()->IncrementSubtotals(time_taken_to_create_graph_,
                                                    time_taken_to_optimize_,
                                                    time_taken_to_codegen_);
  }
}


// Sets the expected number of properties based on estimate from compiler.
void SetExpectedNofPropertiesFromEstimate(Handle<SharedFunctionInfo> shared,
                                          int estimate) {
  // If no properties are added in the constructor, they are more likely
  // to be added later.
  if (estimate == 0) estimate = 2;

  // TODO(yangguo): check whether those heuristics are still up-to-date.
  // We do not shrink objects that go into a snapshot (yet), so we adjust
  // the estimate conservatively.
  if (shared->GetIsolate()->serializer_enabled()) {
    estimate += 2;
  } else {
    // Inobject slack tracking will reclaim redundant inobject space later,
    // so we can afford to adjust the estimate generously.
    estimate += 8;
  }

  shared->set_expected_nof_properties(estimate);
}


static void MaybeDisableOptimization(Handle<SharedFunctionInfo> shared_info,
                                     BailoutReason bailout_reason) {
  if (bailout_reason != kNoReason) {
    shared_info->DisableOptimization(bailout_reason);
  }
}


static void RecordFunctionCompilation(Logger::LogEventsAndTags tag,
                                      CompilationInfo* info,
                                      Handle<SharedFunctionInfo> shared) {
  // SharedFunctionInfo is passed separately, because if CompilationInfo
  // was created using Script object, it will not have it.

  // Log the code generation. If source information is available include
  // script name and line number. Check explicitly whether logging is
  // enabled as finding the line number is not free.
  if (info->isolate()->logger()->is_logging_code_events() ||
      info->isolate()->cpu_profiler()->is_profiling()) {
    Handle<Script> script = info->parse_info()->script();
    Handle<Code> code = info->code();
    if (code.is_identical_to(info->isolate()->builtins()->CompileLazy())) {
      return;
    }
    int line_num = Script::GetLineNumber(script, shared->start_position()) + 1;
    int column_num =
        Script::GetColumnNumber(script, shared->start_position()) + 1;
    String* script_name = script->name()->IsString()
                              ? String::cast(script->name())
                              : info->isolate()->heap()->empty_string();
    Logger::LogEventsAndTags log_tag = Logger::ToNativeByScript(tag, *script);
    PROFILE(info->isolate(),
            CodeCreateEvent(log_tag, *code, *shared, info, script_name,
                            line_num, column_num));
  }
}


static bool CompileUnoptimizedCode(CompilationInfo* info) {
  DCHECK(AllowCompilation::IsAllowed(info->isolate()));
  if (!Compiler::Analyze(info->parse_info()) ||
      !FullCodeGenerator::MakeCode(info)) {
    Isolate* isolate = info->isolate();
    if (!isolate->has_pending_exception()) isolate->StackOverflow();
    return false;
  }
  return true;
}


static bool GenerateBytecode(CompilationInfo* info) {
  DCHECK(AllowCompilation::IsAllowed(info->isolate()));
  if (!Compiler::Analyze(info->parse_info()) ||
      !interpreter::Interpreter::MakeBytecode(info)) {
    Isolate* isolate = info->isolate();
    if (!isolate->has_pending_exception()) isolate->StackOverflow();
    return false;
  }
  return true;
}


MUST_USE_RESULT static MaybeHandle<Code> GetUnoptimizedCodeCommon(
    CompilationInfo* info) {
  VMState<COMPILER> state(info->isolate());
  PostponeInterruptsScope postpone(info->isolate());

  // Parse and update CompilationInfo with the results.
  if (!Parser::ParseStatic(info->parse_info())) return MaybeHandle<Code>();
  Handle<SharedFunctionInfo> shared = info->shared_info();
  FunctionLiteral* lit = info->literal();
  shared->set_language_mode(lit->language_mode());
  SetExpectedNofPropertiesFromEstimate(shared, lit->expected_property_count());
  MaybeDisableOptimization(shared, lit->dont_optimize_reason());

  if (FLAG_ignition && info->closure()->PassesFilter(FLAG_ignition_filter)) {
    // Compile bytecode for the interpreter.
    if (!GenerateBytecode(info)) return MaybeHandle<Code>();
  } else {
    // Compile unoptimized code.
    if (!CompileUnoptimizedCode(info)) return MaybeHandle<Code>();

    CHECK_EQ(Code::FUNCTION, info->code()->kind());
    RecordFunctionCompilation(Logger::LAZY_COMPILE_TAG, info, shared);
  }

  // Update the shared function info with the scope info. Allocating the
  // ScopeInfo object may cause a GC.
  Handle<ScopeInfo> scope_info =
      ScopeInfo::Create(info->isolate(), info->zone(), info->scope());
  shared->set_scope_info(*scope_info);

  // Update the code and feedback vector for the shared function info.
  shared->ReplaceCode(*info->code());
  shared->set_feedback_vector(*info->feedback_vector());

  return info->code();
}


MUST_USE_RESULT static MaybeHandle<Code> GetCodeFromOptimizedCodeMap(
    Handle<JSFunction> function, BailoutId osr_ast_id) {
  Handle<SharedFunctionInfo> shared(function->shared());
  DisallowHeapAllocation no_gc;
  CodeAndLiterals cached = shared->SearchOptimizedCodeMap(
      function->context()->native_context(), osr_ast_id);
  if (cached.code != nullptr) {
    // Caching of optimized code enabled and optimized code found.
    if (cached.literals != nullptr) function->set_literals(cached.literals);
    DCHECK(!cached.code->marked_for_deoptimization());
    DCHECK(function->shared()->is_compiled());
    return Handle<Code>(cached.code);
  }
  return MaybeHandle<Code>();
}


static void InsertCodeIntoOptimizedCodeMap(CompilationInfo* info) {
  Handle<Code> code = info->code();
  if (code->kind() != Code::OPTIMIZED_FUNCTION) return;  // Nothing to do.

  // Context specialization folds-in the context, so no sharing can occur.
  if (info->is_function_context_specializing()) return;
  // Frame specialization implies function context specialization.
  DCHECK(!info->is_frame_specializing());

  // Do not cache bound functions.
  Handle<JSFunction> function = info->closure();
  if (function->shared()->bound()) return;

  // Cache optimized context-specific code.
  if (FLAG_cache_optimized_code) {
    Handle<SharedFunctionInfo> shared(function->shared());
    Handle<LiteralsArray> literals(function->literals());
    Handle<Context> native_context(function->context()->native_context());
    SharedFunctionInfo::AddToOptimizedCodeMap(shared, native_context, code,
                                              literals, info->osr_ast_id());
  }

  // Do not cache context-independent code compiled for OSR.
  if (code->is_turbofanned() && info->is_osr()) return;

  // Cache optimized context-independent code.
  if (FLAG_turbo_cache_shared_code && code->is_turbofanned()) {
    DCHECK(!info->is_function_context_specializing());
    DCHECK(info->osr_ast_id().IsNone());
    Handle<SharedFunctionInfo> shared(function->shared());
    SharedFunctionInfo::AddSharedCodeToOptimizedCodeMap(shared, code);
  }
}


static bool Renumber(ParseInfo* parse_info) {
  if (!AstNumbering::Renumber(parse_info->isolate(), parse_info->zone(),
                              parse_info->literal())) {
    return false;
  }
  Handle<SharedFunctionInfo> shared_info = parse_info->shared_info();
  if (!shared_info.is_null()) {
    FunctionLiteral* lit = parse_info->literal();
    shared_info->set_ast_node_count(lit->ast_node_count());
    MaybeDisableOptimization(shared_info, lit->dont_optimize_reason());
    shared_info->set_dont_crankshaft(lit->flags() &
                                     AstProperties::kDontCrankshaft);
  }
  return true;
}


bool Compiler::Analyze(ParseInfo* info) {
  DCHECK_NOT_NULL(info->literal());
  if (!Rewriter::Rewrite(info)) return false;
  if (!Scope::Analyze(info)) return false;
  if (!Renumber(info)) return false;
  DCHECK_NOT_NULL(info->scope());
  return true;
}


bool Compiler::ParseAndAnalyze(ParseInfo* info) {
  if (!Parser::ParseStatic(info)) return false;
  return Compiler::Analyze(info);
}


static bool GetOptimizedCodeNow(CompilationInfo* info) {
  if (!Compiler::ParseAndAnalyze(info->parse_info())) return false;

  TimerEventScope<TimerEventRecompileSynchronous> timer(info->isolate());

  OptimizedCompileJob job(info);
  if (job.CreateGraph() != OptimizedCompileJob::SUCCEEDED ||
      job.OptimizeGraph() != OptimizedCompileJob::SUCCEEDED ||
      job.GenerateCode() != OptimizedCompileJob::SUCCEEDED) {
    if (FLAG_trace_opt) {
      PrintF("[aborted optimizing ");
      info->closure()->ShortPrint();
      PrintF(" because: %s]\n", GetBailoutReason(info->bailout_reason()));
    }
    return false;
  }

  // Success!
  DCHECK(!info->isolate()->has_pending_exception());
  InsertCodeIntoOptimizedCodeMap(info);
  RecordFunctionCompilation(Logger::LAZY_COMPILE_TAG, info,
                            info->shared_info());
  return true;
}


static bool GetOptimizedCodeLater(CompilationInfo* info) {
  Isolate* isolate = info->isolate();
  if (!isolate->optimizing_compile_dispatcher()->IsQueueAvailable()) {
    if (FLAG_trace_concurrent_recompilation) {
      PrintF("  ** Compilation queue full, will retry optimizing ");
      info->closure()->ShortPrint();
      PrintF(" later.\n");
    }
    return false;
  }

  CompilationHandleScope handle_scope(info);
  if (!Compiler::ParseAndAnalyze(info->parse_info())) return false;

  // Reopen handles in the new CompilationHandleScope.
  info->ReopenHandlesInNewHandleScope();
  info->parse_info()->ReopenHandlesInNewHandleScope();

  TimerEventScope<TimerEventRecompileSynchronous> timer(info->isolate());

  OptimizedCompileJob* job = new (info->zone()) OptimizedCompileJob(info);
  OptimizedCompileJob::Status status = job->CreateGraph();
  if (status != OptimizedCompileJob::SUCCEEDED) return false;
  isolate->optimizing_compile_dispatcher()->QueueForOptimization(job);

  if (FLAG_trace_concurrent_recompilation) {
    PrintF("  ** Queued ");
    info->closure()->ShortPrint();
    if (info->is_osr()) {
      PrintF(" for concurrent OSR at %d.\n", info->osr_ast_id().ToInt());
    } else {
      PrintF(" for concurrent optimization.\n");
    }
  }
  return true;
}


MaybeHandle<Code> Compiler::GetUnoptimizedCode(Handle<JSFunction> function) {
  DCHECK(!function->GetIsolate()->has_pending_exception());
  DCHECK(!function->is_compiled());
  if (function->shared()->is_compiled()) {
    return Handle<Code>(function->shared()->code());
  }

  CompilationInfoWithZone info(function);
  Handle<Code> result;
  ASSIGN_RETURN_ON_EXCEPTION(info.isolate(), result,
                             GetUnoptimizedCodeCommon(&info),
                             Code);
  return result;
}


MaybeHandle<Code> Compiler::GetLazyCode(Handle<JSFunction> function) {
  Isolate* isolate = function->GetIsolate();
  DCHECK(!isolate->has_pending_exception());
  DCHECK(!function->is_compiled());
  AggregatedHistogramTimerScope timer(isolate->counters()->compile_lazy());
  // If the debugger is active, do not compile with turbofan unless we can
  // deopt from turbofan code.
  if (FLAG_turbo_asm && function->shared()->asm_function() &&
      (FLAG_turbo_asm_deoptimization || !isolate->debug()->is_active()) &&
      !FLAG_turbo_osr) {
    CompilationInfoWithZone info(function);

    VMState<COMPILER> state(isolate);
    PostponeInterruptsScope postpone(isolate);

    info.SetOptimizing(BailoutId::None(), handle(function->shared()->code()));

    if (GetOptimizedCodeNow(&info)) {
      DCHECK(function->shared()->is_compiled());
      return info.code();
    }
    // We have failed compilation. If there was an exception clear it so that
    // we can compile unoptimized code.
    if (isolate->has_pending_exception()) isolate->clear_pending_exception();
  }

  if (function->shared()->is_compiled()) {
    return Handle<Code>(function->shared()->code());
  }

  CompilationInfoWithZone info(function);
  Handle<Code> result;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, result, GetUnoptimizedCodeCommon(&info),
                             Code);

  if (FLAG_always_opt) {
    Handle<Code> opt_code;
    if (Compiler::GetOptimizedCode(
            function, result,
            Compiler::NOT_CONCURRENT).ToHandle(&opt_code)) {
      result = opt_code;
    }
  }

  return result;
}


MaybeHandle<Code> Compiler::GetStubCode(Handle<JSFunction> function,
                                        CodeStub* stub) {
  // Build a "hybrid" CompilationInfo for a JSFunction/CodeStub pair.
  Zone zone;
  ParseInfo parse_info(&zone, function);
  CompilationInfo info(&parse_info);
  info.SetFunctionType(stub->GetCallInterfaceDescriptor().GetFunctionType());
  info.MarkAsFunctionContextSpecializing();
  info.MarkAsDeoptimizationEnabled();
  info.SetStub(stub);

  // Run a "mini pipeline", extracted from compiler.cc.
  if (!ParseAndAnalyze(&parse_info)) return MaybeHandle<Code>();
  return compiler::Pipeline(&info).GenerateCode();
}


bool Compiler::Compile(Handle<JSFunction> function, ClearExceptionFlag flag) {
  if (function->is_compiled()) return true;
  MaybeHandle<Code> maybe_code = Compiler::GetLazyCode(function);
  Handle<Code> code;
  if (!maybe_code.ToHandle(&code)) {
    if (flag == CLEAR_EXCEPTION) {
      function->GetIsolate()->clear_pending_exception();
    }
    return false;
  }
  function->ReplaceCode(*code);
  DCHECK(function->is_compiled());
  return true;
}


// TODO(turbofan): In the future, unoptimized code with deopt support could
// be generated lazily once deopt is triggered.
bool Compiler::EnsureDeoptimizationSupport(CompilationInfo* info) {
  DCHECK_NOT_NULL(info->literal());
  DCHECK(info->has_scope());
  Handle<SharedFunctionInfo> shared = info->shared_info();
  if (!shared->has_deoptimization_support()) {
    // TODO(titzer): just reuse the ParseInfo for the unoptimized compile.
    CompilationInfoWithZone unoptimized(info->closure());
    // Note that we use the same AST that we will use for generating the
    // optimized code.
    ParseInfo* parse_info = unoptimized.parse_info();
    parse_info->set_literal(info->literal());
    parse_info->set_scope(info->scope());
    parse_info->set_context(info->context());
    unoptimized.EnableDeoptimizationSupport();
    // If the current code has reloc info for serialization, also include
    // reloc info for serialization for the new code, so that deopt support
    // can be added without losing IC state.
    if (shared->code()->kind() == Code::FUNCTION &&
        shared->code()->has_reloc_info_for_serialization()) {
      unoptimized.PrepareForSerializing();
    }
    if (!FullCodeGenerator::MakeCode(&unoptimized)) return false;

    shared->EnableDeoptimizationSupport(*unoptimized.code());
    shared->set_feedback_vector(*unoptimized.feedback_vector());

    info->MarkAsCompiled();

    // The scope info might not have been set if a lazily compiled
    // function is inlined before being called for the first time.
    if (shared->scope_info() == ScopeInfo::Empty(info->isolate())) {
      Handle<ScopeInfo> target_scope_info =
          ScopeInfo::Create(info->isolate(), info->zone(), info->scope());
      shared->set_scope_info(*target_scope_info);
    }

    // The existing unoptimized code was replaced with the new one.
    RecordFunctionCompilation(Logger::LAZY_COMPILE_TAG, &unoptimized, shared);
  }
  return true;
}


bool CompileEvalForDebugging(Handle<JSFunction> function,
                             Handle<SharedFunctionInfo> shared) {
  Handle<Script> script(Script::cast(shared->script()));
  Handle<Context> context(function->context());

  Zone zone;
  ParseInfo parse_info(&zone, script);
  CompilationInfo info(&parse_info);
  Isolate* isolate = info.isolate();

  parse_info.set_eval();
  parse_info.set_context(context);
  if (context->IsNativeContext()) parse_info.set_global();
  parse_info.set_toplevel();
  parse_info.set_allow_lazy_parsing(false);
  parse_info.set_language_mode(shared->language_mode());
  parse_info.set_parse_restriction(NO_PARSE_RESTRICTION);
  info.MarkAsDebug();

  VMState<COMPILER> state(info.isolate());

  if (!Parser::ParseStatic(&parse_info)) {
    isolate->clear_pending_exception();
    return false;
  }

  FunctionLiteral* lit = parse_info.literal();
  LiveEditFunctionTracker live_edit_tracker(isolate, lit);

  if (!CompileUnoptimizedCode(&info)) {
    isolate->clear_pending_exception();
    return false;
  }
  shared->ReplaceCode(*info.code());
  return true;
}


bool CompileForDebugging(CompilationInfo* info) {
  info->MarkAsDebug();
  if (GetUnoptimizedCodeCommon(info).is_null()) {
    info->isolate()->clear_pending_exception();
    return false;
  }
  return true;
}


static inline bool IsEvalToplevel(Handle<SharedFunctionInfo> shared) {
  return shared->is_toplevel() && shared->script()->IsScript() &&
         Script::cast(shared->script())->compilation_type() ==
             Script::COMPILATION_TYPE_EVAL;
}


bool Compiler::CompileDebugCode(Handle<JSFunction> function) {
  Handle<SharedFunctionInfo> shared(function->shared());
  if (IsEvalToplevel(shared)) {
    return CompileEvalForDebugging(function, shared);
  } else {
    CompilationInfoWithZone info(function);
    return CompileForDebugging(&info);
  }
}


bool Compiler::CompileDebugCode(Handle<SharedFunctionInfo> shared) {
  DCHECK(shared->allows_lazy_compilation_without_context());
  DCHECK(!IsEvalToplevel(shared));
  Zone zone;
  ParseInfo parse_info(&zone, shared);
  CompilationInfo info(&parse_info);
  return CompileForDebugging(&info);
}


void Compiler::CompileForLiveEdit(Handle<Script> script) {
  // TODO(635): support extensions.
  Zone zone;
  ParseInfo parse_info(&zone, script);
  CompilationInfo info(&parse_info);
  PostponeInterruptsScope postpone(info.isolate());
  VMState<COMPILER> state(info.isolate());

  // Get rid of old list of shared function infos.
  info.MarkAsFirstCompile();
  info.parse_info()->set_global();
  if (!Parser::ParseStatic(info.parse_info())) return;

  LiveEditFunctionTracker tracker(info.isolate(), parse_info.literal());
  if (!CompileUnoptimizedCode(&info)) return;
  if (info.has_shared_info()) {
    Handle<ScopeInfo> scope_info =
        ScopeInfo::Create(info.isolate(), info.zone(), info.scope());
    info.shared_info()->set_scope_info(*scope_info);
  }
  tracker.RecordRootFunctionInfo(info.code());
}


static Handle<SharedFunctionInfo> CompileToplevel(CompilationInfo* info) {
  Isolate* isolate = info->isolate();
  PostponeInterruptsScope postpone(isolate);
  DCHECK(!isolate->native_context().is_null());
  ParseInfo* parse_info = info->parse_info();
  Handle<Script> script = parse_info->script();

  // TODO(svenpanne) Obscure place for this, perhaps move to OnBeforeCompile?
  FixedArray* array = isolate->native_context()->embedder_data();
  script->set_context_data(array->get(v8::Context::kDebugIdIndex));

  isolate->debug()->OnBeforeCompile(script);

  DCHECK(parse_info->is_eval() || parse_info->is_global() ||
         parse_info->is_module());

  parse_info->set_toplevel();

  Handle<SharedFunctionInfo> result;

  { VMState<COMPILER> state(info->isolate());
    if (parse_info->literal() == NULL) {
      // Parse the script if needed (if it's already parsed, literal() is
      // non-NULL). If compiling for debugging, we may eagerly compile inner
      // functions, so do not parse lazily in that case.
      ScriptCompiler::CompileOptions options = parse_info->compile_options();
      bool parse_allow_lazy = (options == ScriptCompiler::kConsumeParserCache ||
                               String::cast(script->source())->length() >
                                   FLAG_min_preparse_length) &&
                              !info->is_debug();

      parse_info->set_allow_lazy_parsing(parse_allow_lazy);
      if (!parse_allow_lazy &&
          (options == ScriptCompiler::kProduceParserCache ||
           options == ScriptCompiler::kConsumeParserCache)) {
        // We are going to parse eagerly, but we either 1) have cached data
        // produced by lazy parsing or 2) are asked to generate cached data.
        // Eager parsing cannot benefit from cached data, and producing cached
        // data while parsing eagerly is not implemented.
        parse_info->set_cached_data(nullptr);
        parse_info->set_compile_options(ScriptCompiler::kNoCompileOptions);
      }
      if (!Parser::ParseStatic(parse_info)) {
        return Handle<SharedFunctionInfo>::null();
      }
    }

    DCHECK(!info->is_debug() || !parse_info->allow_lazy_parsing());

    info->MarkAsFirstCompile();

    FunctionLiteral* lit = parse_info->literal();
    LiveEditFunctionTracker live_edit_tracker(isolate, lit);

    // Measure how long it takes to do the compilation; only take the
    // rest of the function into account to avoid overlap with the
    // parsing statistics.
    HistogramTimer* rate = info->is_eval()
          ? info->isolate()->counters()->compile_eval()
          : info->isolate()->counters()->compile();
    HistogramTimerScope timer(rate);

    // Compile the code.
    if (!CompileUnoptimizedCode(info)) {
      return Handle<SharedFunctionInfo>::null();
    }

    // Allocate function.
    DCHECK(!info->code().is_null());
    result = isolate->factory()->NewSharedFunctionInfo(
        lit->name(), lit->materialized_literal_count(), lit->kind(),
        info->code(),
        ScopeInfo::Create(info->isolate(), info->zone(), info->scope()),
        info->feedback_vector());

    DCHECK_EQ(RelocInfo::kNoPosition, lit->function_token_position());
    SharedFunctionInfo::InitFromFunctionLiteral(result, lit);
    SharedFunctionInfo::SetScript(result, script);
    result->set_is_toplevel(true);
    if (info->is_eval()) {
      // Eval scripts cannot be (re-)compiled without context.
      result->set_allows_lazy_compilation_without_context(false);
    }

    Handle<String> script_name = script->name()->IsString()
        ? Handle<String>(String::cast(script->name()))
        : isolate->factory()->empty_string();
    Logger::LogEventsAndTags log_tag = info->is_eval()
        ? Logger::EVAL_TAG
        : Logger::ToNativeByScript(Logger::SCRIPT_TAG, *script);

    PROFILE(isolate, CodeCreateEvent(
                log_tag, *info->code(), *result, info, *script_name));

    // Hint to the runtime system used when allocating space for initial
    // property space by setting the expected number of properties for
    // the instances of the function.
    SetExpectedNofPropertiesFromEstimate(result,
                                         lit->expected_property_count());

    if (!script.is_null())
      script->set_compilation_state(Script::COMPILATION_STATE_COMPILED);

    live_edit_tracker.RecordFunctionInfo(result, lit, info->zone());
  }

  return result;
}


#ifdef SRUK_EVAL_CACHE
EvalCacheManager* EvalCacheManager::mgr_ = NULL;
void EvalCacheManager::PreProcess(Isolate* isolate, Handle<Context> hContext,
      Handle<SharedFunctionInfo> hShared, int codeSize,
      LanguageMode language_mode, int pos) {
#if !defined(V8_TARGET_ARCH_ARM) || !defined(CAN_USE_ARMV7_INSTRUCTIONS)
  return;
#endif

  if (codeSize != codeSize_ || isolate != isolate_ || *hContext != context_
      || language_mode != languageMode_ || pos != scopePosition_
      || codeSize < MaxCodeSize) {
    Clear(isolate);
    isolate_ = isolate;
    context_ = *hContext;
    codeSize_ = codeSize;
    languageMode_ = language_mode;
    scopePosition_ = pos;
  } else {
    count_++;
  }
  if (count_ > EvalCacheThreshold) ready_ = true;
  if (!ready_) return;
  hShared_ = Pop(isolate_);

  if (!hShared_.is_null() && instructionIndex_ == 0) {
    byte* constant1 = NULL;
    Code* code1 = hShared_->code();
    int mode_mask = RelocInfo::ModeMask(RelocInfo::CONST_POOL);
    for (RelocIterator it(code1, mode_mask); !it.done(); it.next()) {
      RelocInfo* rinfo = it.rinfo();
      if (rinfo->IsInConstantPool()) {
        constant1 = rinfo->pc();
        break;
      }
    }
    byte* constant2 = NULL;
    Code* code2 = hShared->code();
    for (RelocIterator it(code2, mode_mask); !it.done(); it.next()) {
      RelocInfo* rinfo = it.rinfo();
      if (rinfo->IsInConstantPool()) {
        constant2 = rinfo->pc();
        break;
      }
    }
    int len1 = constant1 - code1->instruction_start();
    int len2 = constant2 - code2->instruction_start();
    if (len1 == len2) {
      int delta = 0, index = -1;
      uint32_t* array1 = reinterpret_cast<uint32_t *>
                             (code1->instruction_start());
      uint32_t* array2 = reinterpret_cast<uint32_t *>
                             (code2->instruction_start());
      for (int i = 0; i < len1 / kInt32Size ; i++) {
        if (array1[i] != array2[i]) {
          delta++;
          index = i;
        }
      }
      if (delta == 1 && index > 0) {
        int32_t rd0, rs0, rd2, rs2;
        if ((rs0 = ExtractMovImm(&array1[index], &rd0)) &&
            (rs2 = ExtractMovImm(&array2[index], &rd2))) {
          if (rd0 == rd2) {
            instructionIndex_ = index;
            pair_ = false;
            rd_ = rd0;
          }
          int32_t rd1, rs1, rd3, rs3;
          if ((rs1 = ExtractMovImm(&array1[index+1], &rd1)) &&
              (rs3 = ExtractMovImm(&array2[index+1], &rd3))) {
            if ((rs0 == 1) && (rs1 == 2) &&
                (rs2 == 1) && (rs3 == 2) &&
                (rd0 == rd1) && (rd1 == rd3)) {
              pair_ = true;
            }
          }
        }
      }
    }
  }
  Push(isolate_, hShared);
  if (targetString_) {
    delete [] targetString_;
    targetString_ = NULL;
  }
}


bool EvalCacheManager::Process(Isolate* isolate, Handle<Context> hContext,
      Handle<String> src2, LanguageMode language_mode, int pos) {
  activated_ = false;
  if (!ready_ || !hContext->IsNativeContext()) return false;
  if (isolate != isolate_ || *hContext != context_ || Pop(isolate).is_null()
    || language_mode != languageMode_ || pos != scopePosition_) {
    Clear(isolate);
    return false;
  }
  hShared_ = Pop(isolate_);
  if (hShared_.is_null()) return false;

  Handle<String> src1 = handle(String::cast(Script::cast
                                 (hShared_->script())->source()));
  if (src1.is_null() || src2.is_null()) return false;

  int32_t oldValue = GetPropertyValue();
  if (newPropertyNameLength_ >= MAX_NAME_LENGTH) return false;

  uint8_t array[MAX_NAME_LENGTH + 1];
  memcpy(array, newPropertyName_, newPropertyNameLength_);
  array[newPropertyNameLength_] ='\0';
  if (!IsMatchSemantics(src1, src2)) {
    newPropertyNameLength_ = 0;
    return false;
  }

  Handle<Object> value;
  Handle<GlobalObject> global(context_->global_object());
  Handle<String> name = isolate->factory()->InternalizeUtf8String(
                 reinterpret_cast<const char*>(&propertyName_[0]));
  Object::GetProperty(global, name).ToHandle(&value);
  if (!value->IsSmi()) {
    Clear(isolate);
    return false;
  }

  if (numMatchedEvals_ == 0) {
    ++numMatchedEvals_;
    return false;
  }

  if (numMatchedEvals_ > MinMatchedEvals) {
    Handle<Object> value =
            handle(Smi::FromInt(newPropertyValue_), isolate_);
    Handle<String> name = isolate->factory()->InternalizeUtf8String(
               reinterpret_cast<const char*>(newPropertyName_));
    Handle<Object> result;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
    isolate, result,
    Object::SetProperty(global, name, value, languageMode_));

    Handle<Object> v2;
    Object::GetProperty(global, name).ToHandle(&v2);
    if (v2->IsSmi() && Smi::cast(*v2)->value() == newPropertyValue_) {
      Handle<String> name = isolate->factory()->InternalizeUtf8String(
               reinterpret_cast<const char*>(propertyName_));
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result,
      Object::SetProperty(global, name, value, languageMode_));

      if (UpdateNewPropertyValue()) {
        activated_ = true;
        return true;
      }
    }
    Clear(isolate);
    return false;
  }

  Handle<Object> value2;
  name = isolate->factory()->InternalizeUtf8String(
               reinterpret_cast<const char*>(&array[0]));
  Object::GetProperty(global, name).ToHandle(&value2);
  if (value2->IsSmi()) {
    if (Smi::cast(*value2)->value() == oldValue) {
      ++numMatchedEvals_;
      return false;
    }
  }
  Clear(isolate);
  return false;
}


inline static bool IsDigit(uint8_t uc) {
  return (uc >= '0' && uc <= '9');
}


inline static bool IsLetter(uint8_t uc) {
  return (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z');
}


inline static bool IsUnderScore(uint8_t uc) {
  return (uc == '_');
}


static void ConsStringCopy(uint8_t* dest, Handle<String>hString) {
  String* string = *hString;
  uint16_t uc;
  int length = hString->length(), n = 0;

  if (string->IsSeqOneByteString()) {
    memcpy(dest, SeqOneByteString::cast(string)->GetChars(), length);
    dest[length] = '\0';
    return;
  }
  for (int i = 0; i < length; i++) {
    while (true) {
      int index = i;
      if (StringShape(string).IsCons()) {
        ConsString* cons_string = ConsString::cast(string);
        String* left = cons_string->first();
        if (left->length() > index) {
          string = left;
        } else {
          index -= left->length();
          string = cons_string->second();
        }
      } else {
        uc = string->Get(n++);
        if (n >= string->length())  {
          n = 0;
          string = *hString;
        }
        break;
      }
    }
    dest[i] = static_cast<uint8_t>(uc);
  }
  dest[length] = '\0';
}


bool EvalCacheManager::IsNameString(uint8_t* buff, int length, int index,
                         int& start, int &end) {
  uint8_t* tmp = buff + index;
  int count = 0;
  int limit = index > MAX_NAME_LENGTH ? MAX_NAME_LENGTH : index;
  while (1) {
    uint8_t ch = *tmp--;
    if (!IsLetter(ch) && !IsDigit(ch) && !IsUnderScore(ch)) {
      break;
    }
    if (++count >= limit) return false;
  }
  start = index - (count - 1);
  if (!IsLetter(buff[start])) return false;

  count = 0;
  tmp = buff + index;
  limit = (length-index) > MAX_NAME_LENGTH ? MAX_NAME_LENGTH : length;
  while (1) {
    uint8_t ch = *tmp++;
    if (!IsLetter(ch) && !IsDigit(ch) && !IsUnderScore(ch)) {
      break;
    }
    if (++count >= limit) return false;
  }
  end = index + count - 1;
  return true;
}


bool EvalCacheManager::IsValueString(uint8_t* buff, int length,
                          int index, int& start, int &end) {
  uint8_t* tmp = buff + index;
  int count = 0;
  int limit = index > MAX_NAME_LENGTH ? MAX_NAME_LENGTH : index;
  while (IsDigit(*tmp--) && (++count < limit)) {
  }
  start = index - (count - 1);

  count = 0;
  tmp = buff + index;
  limit = (length-index) > MAX_NAME_LENGTH ? MAX_NAME_LENGTH : length;
  while (IsDigit(*tmp++) && (++count < limit)) {
  }
  end = index + count - 1;

  tmp = buff + start;
  for (int i = start; i <= end; i++) {
    if (!IsDigit(*tmp++)) return false;
  }
  uint8_t ch = buff[start-1];
  if (IsLetter(ch)) return false;
  if (IsLetter(buff[end+1])) return false;
  if (ch != ' ' && ch != '=' && ch != '-') return false;
  return true;
}


static bool VerifyIdentifiers(uint8_t* s1, int32_t n1, uint8_t* s2,
                    int32_t n2, uint8_t* s3, int32_t n3, int32_t& m ) {
  if (n3 != n1 && n3 != n2) return false;
  int32_t count = 0;
  if (n3 == n2) {
    if (memcmp(s2, s3, n3) == 0) count++;
  }
  if (n3 == n1) {
    if (memcmp(s1, s3, n3) == 0) count++;
  }
  if (count > 0) {
    m = count;
    return true;
  }
  return false;
}


bool EvalCacheManager::IsMatchSemantics(Handle<String> prevString,
                        Handle<String> currString) {
  // Match previous and current string and return true if they match
  // semantically. Also guarantee that AST tree will match.

  // Check for a flattened cons string
  if (prevString->length() == 0 || currString->length() == 0) return false;
  if (!prevString->IsOneByteRepresentation()
      || !currString->IsOneByteRepresentation()) {
      return false;
  }
  String* string2 = *currString;
  int length1 = prevString->length();
  int length2 = currString->length();
  uint16_t uc1, uc2;
  if (!targetString_) {
    if ((targetString_ = NewArray<uint8_t>(length1+1)) == NULL) {
      return false;
    }
    targetStringLength_ = length1;
    ConsStringCopy(targetString_, prevString);
  }
  if (targetStringLength_ != length1) {
    delete[] targetString_;
    targetString_ = NULL;
    return false;
  }
  uint8_t* buffer = NULL;
  uint8_t* tmpBuffer = NULL;
  base::SmartArrayPointer<uint8_t> array;
  if (currString->IsConsString()) {
    if ((buffer = NewArray<uint8_t>(length2+1)) == NULL) {
      return false;
    }
    ConsStringCopy(buffer, currString);
    array = base::SmartArrayPointer<uint8_t>(buffer);
    tmpBuffer = array.get();
  } else if (string2->IsSeqOneByteString()) {
    tmpBuffer = SeqOneByteString::cast(string2)->GetChars();
  } else {
    return false;
  }
  newPropertyNamePosition_ = 0;
  int nameCount = 0, valueCount = 0;
  int32_t oldStartPos1 = 0, oldEndPos1 = 0;
  int32_t oldStartPos2 = 0, oldEndPos2 = 0;
  int i, j;
  for (i = 0, j = 0; i < length1; i++, j++) {
    uc1 = targetString_[i];
    uc2 = tmpBuffer[j];
    if (uc1 == uc2) { continue; }
    int startPos1, endPos1;
    if (IsNameString(targetString_, length1, i, startPos1, endPos1)) {
      nameCount++;
      int startPos2, endPos2;
      if (!IsNameString(tmpBuffer, length2, j, startPos2, endPos2)) {
        return false;
      }
      if (nameCount > 1) {
        if (nameCount > 2) {
          uint8_t* tmp1 = targetString_ + propertyNamePosition_;
          uint8_t* tmp2 = targetString_ + oldStartPos1;
          uint8_t* tmp3 = targetString_ + startPos1;
          int32_t n1 = propertyNameLength_;
          int32_t n2 = oldEndPos1 - oldStartPos1 + 1;
          int32_t n3 = endPos1 - startPos1 + 1;
          int32_t marker1;
          if (!VerifyIdentifiers(tmp1, n1, tmp2, n2, tmp3, n3, marker1)) {
              return false;
          }
          tmp1 = tmpBuffer + newPropertyNamePosition_;
          tmp2 = tmpBuffer + oldStartPos2;
          tmp3 = tmpBuffer + startPos2;
          n1 = newPropertyNameLength_;
          n2 = oldEndPos2 - oldStartPos2 + 1;
          n3 = endPos2 - startPos2 + 1;
          int32_t marker2;
          if (!VerifyIdentifiers(tmp1, n1, tmp2, n2, tmp3, n3, marker2)) {
              return false;
          }
          if (marker1 != marker2) {
            return false;
          }
        }
        oldStartPos1 = startPos1; oldEndPos1 = endPos1;
        oldStartPos2 = startPos2; oldEndPos2 = endPos2;
      } else {
        propertyNamePosition_ = startPos1;
        propertyNameLength_ = endPos1 - startPos1 + 1;
        newPropertyNamePosition_ = startPos2;
        newPropertyNameLength_ = endPos2 - startPos2 + 1;
      }
      i = endPos1; j = endPos2;
    } else if (IsValueString(targetString_, length1, i, startPos1, endPos1)) {
      valueCount++;
      if (valueCount > 1) return false;
      int startPos2, endPos2;
      if (!IsValueString(tmpBuffer, length2, j, startPos2, endPos2)) {
        return false;
      }
      if (targetString_[endPos1 + 1] != ' '
          && targetString_[endPos1 + 1] != ';')
          return false;
      if (tmpBuffer[endPos2 + 1] != ' ' && tmpBuffer[endPos2 + 1] != ';')
          return false;
      int factor = 1;
      uint8_t* tmpPtr = tmpBuffer + startPos2 -1;
      if (tmpBuffer[startPos2 - 1] == '-') { factor = -1; tmpPtr--; }
      uint8_t ch;
      while ((ch = *tmpPtr--) == ' ' && (tmpPtr > tmpBuffer));
      if (tmpPtr <= tmpBuffer || ch != '=') return false;
      if (newPropertyNamePosition_ > 0) {
        tmpPtr = tmpBuffer + newPropertyNamePosition_ - 1;
        while ((ch = *tmpPtr--) == ' ' && (tmpPtr > tmpBuffer));
        if (tmpPtr < tmpBuffer+1 || ch != 'r' || *tmpPtr-- != 'a'
          || *tmpPtr != 'v')
          return false;
      } else {
        while ((ch = *tmpPtr--) == ' ' && (tmpPtr > tmpBuffer));
        if (tmpPtr <= tmpBuffer || !IsLetter(ch)) return false;
        int x, y;
        if (!IsNameString(tmpBuffer, length2, tmpPtr-tmpBuffer+1, x, y))
            return false;
        tmpPtr = tmpBuffer + x - 1;
        while ((ch = *tmpPtr--) == ' ' && (tmpPtr > tmpBuffer));
        if (tmpPtr < tmpBuffer+1 || ch != 'r' || *tmpPtr-- != 'a'
          || *tmpPtr != 'v')
          return false;
        propertyNamePosition_ = x;
        propertyNameLength_ = y - x + 1;
        newPropertyNamePosition_ = x;
        newPropertyNameLength_ = y - x + 1;
      }
      if (tmpPtr != tmpBuffer) {
        ch = *(tmpPtr-1);
        if (IsLetter(ch) || IsDigit(ch) || ch == '.' || ch =='_')
            return false;
      }
      int len2 = endPos2- startPos2 + 1;
      char *plate = new char[len2 + 1];
      memcpy(plate, tmpBuffer + startPos2, len2);
      plate[len2] = '\0';
      newPropertyValue_ = atoi(plate);
      if (newPropertyValue_ > (1 << 28)) return false;
      newPropertyValue_ *= factor;
      i = endPos1; j = endPos2;
    } else {
      return false;
    }
  }  // for loop
  if (i != length1 || j != length2) return false;
  if (valueCount != 1 || newPropertyNamePosition_ == 0) return false;
  if (propertyNameLength_ >= MAX_NAME_LENGTH) return false;
  if (propertyName_ == NULL) {
    propertyName_ = NewArray<uint8_t>(MAX_NAME_LENGTH + 1);
    if (propertyName_ == NULL) return false;
  }
  memcpy(propertyName_, targetString_ + propertyNamePosition_,
         propertyNameLength_);
  propertyName_[propertyNameLength_] = '\0';

  if (newPropertyNameLength_ >= MAX_NAME_LENGTH) return false;
  if (newPropertyName_ == NULL) {
    newPropertyName_ = NewArray<uint8_t>(MAX_NAME_LENGTH + 1);
    if (newPropertyName_ == NULL) return false;
  }
  memcpy(newPropertyName_, tmpBuffer + newPropertyNamePosition_,
         newPropertyNameLength_);
  newPropertyName_[newPropertyNameLength_] = '\0';

  return true;
}


bool EvalCacheManager::MakeMoveImmediate(uint32_t value, int32_t rd,
         uint32_t& out, uint32_t& out1) {
#ifdef V8_TARGET_ARCH_ARM
  if (!pair_) {
    if (value >= 0x10000) return false;
    out = ARM_MOVW_OPCODE*B20 | rd*B12 | ((value & 0xf000)*B4) |
        (value & 0xfff);
  } else {
    out = ARM_MOVW_OPCODE*B20 | rd*B12 | ((value & 0xf000)*B4) |
        (value & 0xfff);
    value >>= 16;
    out1 = ARM_MOVT_OPCODE*B20 | rd*B12 | ((value & 0xf000)*B4) |
        (value & 0xfff);
  }
  return true;
#else
  return false;
#endif
}


int32_t EvalCacheManager::ExtractMovImm(uint32_t* op, int32_t* rd) {
  int ret = 0;
  Instruction* instr = reinterpret_cast<Instruction*>(op);
  uint32_t type = instr->TypeValue();
  if (type == 1) {
    switch (instr->OpcodeField()) {
      case TST: {
        if (!instr->HasS()) {
          Condition condf = instr->ConditionField();
          if (condf == Condition::al) {
            *rd = instr->RdValue();
            ret = 1;  // movw
          }
        }
        break;
      }
      case CMP: {
        if (!instr->HasS()) {
          Condition condf = instr->ConditionField();
          if (condf == Condition::al) {
            *rd = instr->RdValue();
            ret = 2;  // movt
          }
        }
        break;
      }
      case MOV: {
        if (!instr->HasS()) {
          Condition condf = instr->ConditionField();
          if (condf == Condition::al) {
            *rd = instr->RdValue();
            ret = 3;  // mov
          }
        }
        break;
      }
      default: break;
    }
  }
  return ret;
}


bool EvalCacheManager::UpdateNewPropertyValue() {
  hShared_ = Pop(isolate_);
  if (hShared_.is_null() || instructionIndex_ == 0) return false;
  Code* code = hShared_->code();
  uint32_t* array = reinterpret_cast<uint32_t*>(code->instruction_start());
  uint32_t out;
  uint32_t out1 = array[instructionIndex_ + 1];
  int32_t rd0;
  int32_t rs0;
  if ((rs0 = ExtractMovImm(&array[instructionIndex_], &rd0)) == 0) {
    return false;
  }
  if (rd0 != rd_) return false;
  int32_t rd1;
  int32_t rs1;
  bool pair = false;
  if ((rs1 = ExtractMovImm(&array[instructionIndex_ + 1], &rd1))) {
    if ((rs0 == 1) && (rs1 == 2) && (rd0 == rd1)) {
      pair = true;
    }
  }
  if ((rs0 == 3) && (rs1 == 2)) return false;
  if (pair != pair_) return false;
  if (!MakeMoveImmediate(
        reinterpret_cast<uint32_t>(Smi::FromInt(newPropertyValue_)),
        rd0, out, out1)) {
    return false;
  }

  array[instructionIndex_] = out;

  if (pair == true) {
    array[instructionIndex_ + 1] = out1;
  }
  CpuFeatures::FlushICache(&array[instructionIndex_], 2*kPointerSize);
  return true;
}
#endif


MaybeHandle<JSFunction> Compiler::GetFunctionFromEval(
    Handle<String> source, Handle<SharedFunctionInfo> outer_info,
    Handle<Context> context, LanguageMode language_mode,
    ParseRestriction restriction, int line_offset, int column_offset,
    Handle<Object> script_name, ScriptOriginOptions options) {
  Isolate* isolate = source->GetIsolate();
  int source_length = source->length();
  isolate->counters()->total_eval_size()->Increment(source_length);
  isolate->counters()->total_compile_size()->Increment(source_length);

  CompilationCache* compilation_cache = isolate->compilation_cache();
  MaybeHandle<SharedFunctionInfo> maybe_shared_info =
      compilation_cache->LookupEval(source, outer_info, context, language_mode,
                                    line_offset);
  Handle<SharedFunctionInfo> shared_info;

  Handle<Script> script;
#ifdef SRUK_EVAL_CACHE
  EvalCacheManager* mgr = EvalCacheManager::GetInstance();
  if (mgr->MaybeReady() && !maybe_shared_info.ToHandle(&shared_info)) {
    shared_info = mgr->Pop(isolate);
    if (mgr->Process(isolate, context, source, language_mode, line_offset)) {
      if (shared_info->ic_age() != isolate->heap()->global_ic_age()) {
        shared_info->ResetForNewContext(isolate->heap()->global_ic_age());
      }
      return isolate->factory()->NewFunctionFromSharedFunctionInfo(
          shared_info, context, NOT_TENURED);
    }
  }
#endif

  if (!maybe_shared_info.ToHandle(&shared_info)) {
    script = isolate->factory()->NewScript(source);
    if (!script_name.is_null()) {
      script->set_name(*script_name);
      script->set_line_offset(line_offset);
      script->set_column_offset(column_offset);
    }
    script->set_origin_options(options);
    Zone zone;
    ParseInfo parse_info(&zone, script);
    CompilationInfo info(&parse_info);
    parse_info.set_eval();
    if (context->IsNativeContext()) parse_info.set_global();
    parse_info.set_language_mode(language_mode);
    parse_info.set_parse_restriction(restriction);
    parse_info.set_context(context);

    Debug::RecordEvalCaller(script);

    shared_info = CompileToplevel(&info);

    if (shared_info.is_null()) {
      return MaybeHandle<JSFunction>();
    } else {
      // Explicitly disable optimization for eval code. We're not yet prepared
      // to handle eval-code in the optimizing compiler.
      if (restriction != ONLY_SINGLE_FUNCTION_LITERAL) {
        shared_info->DisableOptimization(kEval);
      }

      // If caller is strict mode, the result must be in strict mode as well.
      DCHECK(is_sloppy(language_mode) ||
             is_strict(shared_info->language_mode()));
      compilation_cache->PutEval(source, outer_info, context, shared_info,
                                   line_offset);
#ifdef SRUK_EVAL_CACHE
        if (context->IsNativeContext() && shared_info->code()
            && shared_info->script()) {
          mgr->PreProcess(isolate, context, shared_info,
              shared_info->code()->body_size(), language_mode, line_offset);
        }
#endif
    }
  } else if (shared_info->ic_age() != isolate->heap()->global_ic_age()) {
    shared_info->ResetForNewContext(isolate->heap()->global_ic_age());
  }

  Handle<JSFunction> result =
      isolate->factory()->NewFunctionFromSharedFunctionInfo(
          shared_info, context, NOT_TENURED);

  // OnAfterCompile has to be called after we create the JSFunction, which we
  // may require to recompile the eval for debugging, if we find a function
  // that contains break points in the eval script.
  isolate->debug()->OnAfterCompile(script);

  return result;
}


CodeShareManager* CodeShareManager::manager_ = NULL;


void CodeShareManager::Process(
    Isolate* isolate,
    Handle<Context> hContext,
    Handle<String> hSource, Handle<Object> name) {
  if (IsActivated() && contextNew_ == 0
      && ++intervalCount_ > expectedInterval_) {
    CleanUp();
    return;
  }
  keyIndex_++;
  if (!contextNew_) return;
  contextNew_ = false;
  intervalCount_ = 0;
  if (!IsReady() || name.is_null() || !name->IsString()
      || hSource->IsConsString()
      || (!hSource->IsSeqOneByteString()
          && !hSource->IsExternalString()
          && !hSource->IsExternalOneByteString())) {
    numItems_ = 0;
    activated_ = false;
    return;
  }
  if (!IsActivated()) {
    if (numItems_ == 0) {
      isolate_ = isolate;
      Clear();
      isolate_ = isolate;  // save isolate ptr
      name_ = *name;  // save name ptr
    }
    if (numItems_ >=  MAX_NUM_ITEMS) numItems_ = 0;
    Element elem;
    elem.context = *hContext;
    elem.length = hSource->length();
    if (hSource->IsSeqOneByteString()) {
      elem.source = SeqOneByteString::cast(*hSource)->GetChars();
    } else if (hSource->IsExternalOneByteString()) {
      elem.source = ExternalOneByteString::cast(*hSource)->GetChars();
    } else {
      void* addr = (reinterpret_cast<uint8_t *>(*hSource) +
                    ExternalString::kResourceOffset - kHeapObjectTag);
      uint8_t** ptr = reinterpret_cast<uint8_t**>(addr);
      elem.source = *ptr;
    }
    elem.key = keyIndex_;
    array_[numItems_++] = elem;
    CodeSharingCache::Enter(isolate_, hContext, numItems_-1);
    int index = Find(elem);
    int distance = (numItems_ - index) - 1;
    if (index >= 0 && distance > MIN_NUM_SCRIPTS) {
      frameOffset_ = index + 1;
      frameLength_ = distance;
      id_ = frameLength_ - 1;
      for (int i = 0; i < frameOffset_; i++) {
        CodeSharingCache::Clear(isolate_, i);
      }
      for (int i = frameOffset_ + frameLength_; i < numItems_; i++) {
        CodeSharingCache::Clear(isolate_, i);
      }
      SetActivated();
      expectedInterval_ = array_[frameOffset_+1].key
                            - array_[frameOffset_].key - 1;
    }
  }
  if (IsActivated()) {
    if (++id_ >= frameLength_) id_ = 0;
    Element elem;
    elem.context = *hContext;
    elem.length = hSource->length();
    if (hSource->IsSeqOneByteString()) {
      elem.source = SeqOneByteString::cast(*hSource)->GetChars();
    } else if (hSource->IsExternalOneByteString()) {
      elem.source = ExternalOneByteString::cast(*hSource)->GetChars();
    } else {
      void* addr = (reinterpret_cast<uint8_t *>(*hSource) +
                  ExternalString::kResourceOffset - kHeapObjectTag);
      uint8_t** ptr = reinterpret_cast<uint8_t**>(addr);
      elem.source = *ptr;
    }
    elem.key = 0;
    if (isolate != isolate_ || name_ != *name || Find(elem) < 0) CleanUp();
  }
  return;
}


Handle<Context> CodeShareManager::Pop(Isolate* isolate) {
    if (IsActivated()) {
      if (isolate != isolate_) {
        CleanUp();
        return Handle<Context>::null();
      }
      int pos = (frameOffset_ + id_);
      Handle<Context> env = CodeSharingCache::Lookup(isolate_, pos);
      if (!env.is_null() && isolate->context()
          && isolate->context()->IsContext()
          && isolate->context()->IsNativeContext()) {
        return env;
      } else {
        CleanUp();
        return Handle<Context>::null();
      }
    }
    return Handle<Context>::null();
}


int CodeShareManager::Find(Element& elem) {
  if (numItems_ < MIN_NUM_SCRIPTS) return -1;
  if (!activated_) {
    int index = -1;
    for (int i = numItems_-2; i >= 0; i--) {
      if (array_[i].length == elem.length
          && (array_[i].source == elem.source
              || !memcmp(array_[i].source, elem.source, elem.length))
          && array_[i].context != elem.context) {
        index = i;
        break;
      }
    }
    if (index >= 0) {
      for (int i = 1; i < numItems_-1; i++) {
        if ((array_[i+1].key - array_[i].key)
          != (array_[i].key - array_[i-1].key)) {
          return -1;
        }
      }
      return index;
    }
  } else {
    for (int i = frameOffset_; i < frameOffset_ + frameLength_; i++) {
      if (array_[i].length == elem.length &&
          !memcmp(array_[i].source, elem.source, elem.length)) {
        return i;
      }
    }
  }
  return -1;
}


Handle<SharedFunctionInfo> Compiler::CompileScript(
    Handle<String> source, Handle<Object> script_name, int line_offset,
    int column_offset, ScriptOriginOptions resource_options,
    Handle<Object> source_map_url, Handle<Context> context,
    v8::Extension* extension, ScriptData** cached_data,
    ScriptCompiler::CompileOptions compile_options, NativesFlag natives,
    bool is_module) {
  Isolate* isolate = source->GetIsolate();
  if (compile_options == ScriptCompiler::kNoCompileOptions) {
    cached_data = NULL;
  } else if (compile_options == ScriptCompiler::kProduceParserCache ||
             compile_options == ScriptCompiler::kProduceCodeCache) {
    DCHECK(cached_data && !*cached_data);
    DCHECK(extension == NULL);
    DCHECK(!isolate->debug()->is_loaded());
  } else {
    DCHECK(compile_options == ScriptCompiler::kConsumeParserCache ||
           compile_options == ScriptCompiler::kConsumeCodeCache);
    DCHECK(cached_data && *cached_data);
    DCHECK(extension == NULL);
  }
  int source_length = source->length();
  isolate->counters()->total_load_size()->Increment(source_length);
  isolate->counters()->total_compile_size()->Increment(source_length);

  // TODO(rossberg): The natives do not yet obey strong mode rules
  // (for example, some macros use '==').
  bool use_strong = FLAG_use_strong && !isolate->bootstrapper()->IsActive();
  LanguageMode language_mode =
      construct_language_mode(FLAG_use_strict, use_strong);

  CodeShareManager::GetInstance()->Process(isolate,
                         isolate->native_context(), source, script_name);

  CompilationCache* compilation_cache = isolate->compilation_cache();

  // Do a lookup in the compilation cache but not for extensions.
  MaybeHandle<SharedFunctionInfo> maybe_result;
  Handle<SharedFunctionInfo> result;
  if (extension == NULL) {
    // First check per-isolate compilation cache.
    maybe_result = compilation_cache->LookupScript(
        source, script_name, line_offset, column_offset, resource_options,
        context, language_mode);
    if (maybe_result.is_null() && FLAG_serialize_toplevel &&
        compile_options == ScriptCompiler::kConsumeCodeCache &&
        !isolate->debug()->is_loaded()) {
      // Then check cached code provided by embedder.
      HistogramTimerScope timer(isolate->counters()->compile_deserialize());
      Handle<SharedFunctionInfo> result;
      if (CodeSerializer::Deserialize(isolate, *cached_data, source)
              .ToHandle(&result)) {
        // Promote to per-isolate compilation cache.
        compilation_cache->PutScript(source, context, language_mode, result);
        return result;
      }
      // Deserializer failed. Fall through to compile.
    }
  }

  base::ElapsedTimer timer;
  if (FLAG_profile_deserialization && FLAG_serialize_toplevel &&
      compile_options == ScriptCompiler::kProduceCodeCache) {
    timer.Start();
  }

  if (!maybe_result.ToHandle(&result)) {
    // No cache entry found. Compile the script.

    // Create a script object describing the script to be compiled.
    Handle<Script> script = isolate->factory()->NewScript(source);
    if (natives == NATIVES_CODE) {
      script->set_type(Script::TYPE_NATIVE);
      script->set_hide_source(true);
    }
    if (!script_name.is_null()) {
      script->set_name(*script_name);
      script->set_line_offset(line_offset);
      script->set_column_offset(column_offset);
    }
    script->set_origin_options(resource_options);
    if (!source_map_url.is_null()) {
      script->set_source_mapping_url(*source_map_url);
    }

    // Compile the function and add it to the cache.
    Zone zone;
    ParseInfo parse_info(&zone, script);
    CompilationInfo info(&parse_info);
    if (FLAG_harmony_modules && is_module) {
      parse_info.set_module();
    } else {
      parse_info.set_global();
    }
    if (compile_options != ScriptCompiler::kNoCompileOptions) {
      parse_info.set_cached_data(cached_data);
    }
    parse_info.set_compile_options(compile_options);
    parse_info.set_extension(extension);
    parse_info.set_context(context);
    if (FLAG_serialize_toplevel &&
        compile_options == ScriptCompiler::kProduceCodeCache) {
      info.PrepareForSerializing();
    }

    parse_info.set_language_mode(
        static_cast<LanguageMode>(info.language_mode() | language_mode));
    result = CompileToplevel(&info);
    if (extension == NULL && !result.is_null()) {
      compilation_cache->PutScript(source, context, language_mode, result);
      if (FLAG_serialize_toplevel &&
          compile_options == ScriptCompiler::kProduceCodeCache) {
        HistogramTimerScope histogram_timer(
            isolate->counters()->compile_serialize());
        *cached_data = CodeSerializer::Serialize(isolate, result, source);
        if (FLAG_profile_deserialization) {
          PrintF("[Compiling and serializing took %0.3f ms]\n",
                 timer.Elapsed().InMillisecondsF());
        }
      }
    }

    if (result.is_null()) {
      isolate->ReportPendingMessages();
    } else {
      isolate->debug()->OnAfterCompile(script);
    }
  } else if (result->ic_age() != isolate->heap()->global_ic_age()) {
    result->ResetForNewContext(isolate->heap()->global_ic_age());
  }
  return result;
}


Handle<SharedFunctionInfo> Compiler::CompileStreamedScript(
    Handle<Script> script, ParseInfo* parse_info, int source_length) {
  Isolate* isolate = script->GetIsolate();
  // TODO(titzer): increment the counters in caller.
  isolate->counters()->total_load_size()->Increment(source_length);
  isolate->counters()->total_compile_size()->Increment(source_length);

  LanguageMode language_mode =
      construct_language_mode(FLAG_use_strict, FLAG_use_strong);
  parse_info->set_language_mode(
      static_cast<LanguageMode>(parse_info->language_mode() | language_mode));

  CompilationInfo compile_info(parse_info);

  // The source was parsed lazily, so compiling for debugging is not possible.
  DCHECK(!compile_info.is_debug());

  Handle<SharedFunctionInfo> result = CompileToplevel(&compile_info);
  if (!result.is_null()) isolate->debug()->OnAfterCompile(script);
  return result;
}


Handle<SharedFunctionInfo> Compiler::GetSharedFunctionInfo(
    FunctionLiteral* literal, Handle<Script> script,
    CompilationInfo* outer_info) {
  // Precondition: code has been parsed and scopes have been analyzed.
  Isolate* isolate = outer_info->isolate();
  MaybeHandle<SharedFunctionInfo> maybe_existing;
  if (outer_info->is_first_compile()) {
    // On the first compile, there are no existing shared function info for
    // inner functions yet, so do not try to find them. All bets are off for
    // live edit though.
    DCHECK(script->FindSharedFunctionInfo(literal).is_null() ||
           isolate->debug()->live_edit_enabled());
  } else {
    maybe_existing = script->FindSharedFunctionInfo(literal);
  }
  // We found an existing shared function info. If it's already compiled,
  // don't worry about compiling it, and simply return it. If it's not yet
  // compiled, continue to decide whether to eagerly compile.
  // Carry on if we are compiling eager to obtain code for debugging,
  // unless we already have code with debut break slots.
  Handle<SharedFunctionInfo> existing;
  if (maybe_existing.ToHandle(&existing) && existing->is_compiled()) {
    if (!outer_info->is_debug() || existing->HasDebugCode()) {
      return existing;
    }
  }

  Zone zone;
  ParseInfo parse_info(&zone, script);
  CompilationInfo info(&parse_info);
  parse_info.set_literal(literal);
  parse_info.set_scope(literal->scope());
  parse_info.set_language_mode(literal->scope()->language_mode());
  if (outer_info->will_serialize()) info.PrepareForSerializing();
  if (outer_info->is_first_compile()) info.MarkAsFirstCompile();
  if (outer_info->is_debug()) info.MarkAsDebug();

  LiveEditFunctionTracker live_edit_tracker(isolate, literal);
  // Determine if the function can be lazily compiled. This is necessary to
  // allow some of our builtin JS files to be lazily compiled. These
  // builtins cannot be handled lazily by the parser, since we have to know
  // if a function uses the special natives syntax, which is something the
  // parser records.
  // If the debugger requests compilation for break points, we cannot be
  // aggressive about lazy compilation, because it might trigger compilation
  // of functions without an outer context when setting a breakpoint through
  // Debug::FindSharedFunctionInfoInScript.
  bool allow_lazy_without_ctx = literal->AllowsLazyCompilationWithoutContext();
  // Compile eagerly for live edit. When compiling debug code, eagerly compile
  // unless we can lazily compile without the context.
  bool allow_lazy = literal->AllowsLazyCompilation() &&
                    !LiveEditFunctionTracker::IsActive(isolate) &&
                    (!info.is_debug() || allow_lazy_without_ctx);

  if (outer_info->parse_info()->is_toplevel() && outer_info->will_serialize()) {
    // Make sure that if the toplevel code (possibly to be serialized),
    // the inner function must be allowed to be compiled lazily.
    // This is necessary to serialize toplevel code without inner functions.
    DCHECK(allow_lazy);
  }

  bool lazy = FLAG_lazy && allow_lazy && !literal->should_eager_compile();

  // Generate code
  Handle<ScopeInfo> scope_info;
  if (lazy) {
    Handle<Code> code = isolate->builtins()->CompileLazy();
    info.SetCode(code);
    // There's no need in theory for a lazy-compiled function to have a type
    // feedback vector, but some parts of the system expect all
    // SharedFunctionInfo instances to have one.  The size of the vector depends
    // on how many feedback-needing nodes are in the tree, and when lazily
    // parsing we might not know that, if this function was never parsed before.
    // In that case the vector will be replaced the next time MakeCode is
    // called.
    info.EnsureFeedbackVector();
    scope_info = Handle<ScopeInfo>(ScopeInfo::Empty(isolate));
  } else if (Renumber(info.parse_info()) &&
             FullCodeGenerator::MakeCode(&info)) {
    // MakeCode will ensure that the feedback vector is present and
    // appropriately sized.
    DCHECK(!info.code().is_null());
    scope_info = ScopeInfo::Create(info.isolate(), info.zone(), info.scope());
    if (literal->should_eager_compile() &&
        literal->should_be_used_once_hint()) {
      info.code()->MarkToBeExecutedOnce(isolate);
    }
  } else {
    return Handle<SharedFunctionInfo>::null();
  }

  if (maybe_existing.is_null()) {
    // Create a shared function info object.
    Handle<SharedFunctionInfo> result =
        isolate->factory()->NewSharedFunctionInfo(
            literal->name(), literal->materialized_literal_count(),
            literal->kind(), info.code(), scope_info, info.feedback_vector());

    SharedFunctionInfo::InitFromFunctionLiteral(result, literal);
    SharedFunctionInfo::SetScript(result, script);
    result->set_is_toplevel(false);
    // If the outer function has been compiled before, we cannot be sure that
    // shared function info for this function literal has been created for the
    // first time. It may have already been compiled previously.
    result->set_never_compiled(outer_info->is_first_compile() && lazy);

    RecordFunctionCompilation(Logger::FUNCTION_TAG, &info, result);
    result->set_allows_lazy_compilation(literal->AllowsLazyCompilation());
    result->set_allows_lazy_compilation_without_context(allow_lazy_without_ctx);

    // Set the expected number of properties for instances and return
    // the resulting function.
    SetExpectedNofPropertiesFromEstimate(result,
                                         literal->expected_property_count());
    live_edit_tracker.RecordFunctionInfo(result, literal, info.zone());
    return result;
  } else if (!lazy) {
    // Assert that we are not overwriting (possibly patched) debug code.
    DCHECK(!existing->HasDebugCode());
    existing->ReplaceCode(*info.code());
    existing->set_scope_info(*scope_info);
    existing->set_feedback_vector(*info.feedback_vector());
  }
  return existing;
}


MaybeHandle<Code> Compiler::GetOptimizedCode(Handle<JSFunction> function,
                                             Handle<Code> current_code,
                                             ConcurrencyMode mode,
                                             BailoutId osr_ast_id,
                                             JavaScriptFrame* osr_frame) {
  Isolate* isolate = function->GetIsolate();
  Handle<SharedFunctionInfo> shared(function->shared(), isolate);
  if (shared->HasDebugInfo()) return MaybeHandle<Code>();

  Handle<Code> cached_code;
  if (GetCodeFromOptimizedCodeMap(
          function, osr_ast_id).ToHandle(&cached_code)) {
    if (FLAG_trace_opt) {
      PrintF("[found optimized code for ");
      function->ShortPrint();
      if (!osr_ast_id.IsNone()) {
        PrintF(" at OSR AST id %d", osr_ast_id.ToInt());
      }
      PrintF("]\n");
    }
    return cached_code;
  }

  DCHECK(AllowCompilation::IsAllowed(isolate));

  if (!shared->is_compiled() ||
      shared->scope_info() == ScopeInfo::Empty(isolate)) {
    // The function was never compiled. Compile it unoptimized first.
    // TODO(titzer): reuse the AST and scope info from this compile.
    CompilationInfoWithZone unoptimized(function);
    unoptimized.EnableDeoptimizationSupport();
    if (!GetUnoptimizedCodeCommon(&unoptimized).ToHandle(&current_code)) {
      return MaybeHandle<Code>();
    }
    shared->ReplaceCode(*current_code);
  }

  current_code->set_profiler_ticks(0);

  // TODO(mstarzinger): We cannot properly deserialize a scope chain containing
  // an eval scope and hence would fail at parsing the eval source again.
  if (shared->disable_optimization_reason() == kEval) {
    return MaybeHandle<Code>();
  }

  // TODO(mstarzinger): We cannot properly deserialize a scope chain for the
  // builtin context, hence Genesis::InstallExperimentalNatives would fail.
  if (shared->is_toplevel() && isolate->bootstrapper()->IsActive()) {
    return MaybeHandle<Code>();
  }

  base::SmartPointer<CompilationInfo> info(
      new CompilationInfoWithZone(function));
  VMState<COMPILER> state(isolate);
  DCHECK(!isolate->has_pending_exception());
  PostponeInterruptsScope postpone(isolate);

  info->SetOptimizing(osr_ast_id, current_code);

  if (mode == CONCURRENT) {
    if (GetOptimizedCodeLater(info.get())) {
      info.Detach();  // The background recompile job owns this now.
      return isolate->builtins()->InOptimizationQueue();
    }
  } else {
    info->set_osr_frame(osr_frame);
    if (GetOptimizedCodeNow(info.get())) return info->code();
  }

  if (isolate->has_pending_exception()) isolate->clear_pending_exception();
  return MaybeHandle<Code>();
}


Handle<Code> Compiler::GetConcurrentlyOptimizedCode(OptimizedCompileJob* job) {
  // Take ownership of compilation info.  Deleting compilation info
  // also tears down the zone and the recompile job.
  base::SmartPointer<CompilationInfo> info(job->info());
  Isolate* isolate = info->isolate();

  VMState<COMPILER> state(isolate);
  TimerEventScope<TimerEventRecompileSynchronous> timer(info->isolate());

  Handle<SharedFunctionInfo> shared = info->shared_info();
  shared->code()->set_profiler_ticks(0);

  DCHECK(!shared->HasDebugInfo());

  // 1) Optimization on the concurrent thread may have failed.
  // 2) The function may have already been optimized by OSR.  Simply continue.
  //    Except when OSR already disabled optimization for some reason.
  // 3) The code may have already been invalidated due to dependency change.
  // 4) Code generation may have failed.
  if (job->last_status() == OptimizedCompileJob::SUCCEEDED) {
    if (shared->optimization_disabled()) {
      job->RetryOptimization(kOptimizationDisabled);
    } else if (info->dependencies()->HasAborted()) {
      job->RetryOptimization(kBailedOutDueToDependencyChange);
    } else if (job->GenerateCode() == OptimizedCompileJob::SUCCEEDED) {
      RecordFunctionCompilation(Logger::LAZY_COMPILE_TAG, info.get(), shared);
      if (shared->SearchOptimizedCodeMap(info->context()->native_context(),
                                         info->osr_ast_id()).code == nullptr) {
        InsertCodeIntoOptimizedCodeMap(info.get());
      }
      if (FLAG_trace_opt) {
        PrintF("[completed optimizing ");
        info->closure()->ShortPrint();
        PrintF("]\n");
      }
      return Handle<Code>(*info->code());
    }
  }

  DCHECK(job->last_status() != OptimizedCompileJob::SUCCEEDED);
  if (FLAG_trace_opt) {
    PrintF("[aborted optimizing ");
    info->closure()->ShortPrint();
    PrintF(" because: %s]\n", GetBailoutReason(info->bailout_reason()));
  }
  return Handle<Code>::null();
}


CompilationPhase::CompilationPhase(const char* name, CompilationInfo* info)
    : name_(name), info_(info) {
  if (FLAG_hydrogen_stats) {
    info_zone_start_allocation_size_ = info->zone()->allocation_size();
    timer_.Start();
  }
}


CompilationPhase::~CompilationPhase() {
  if (FLAG_hydrogen_stats) {
    size_t size = zone()->allocation_size();
    size += info_->zone()->allocation_size() - info_zone_start_allocation_size_;
    isolate()->GetHStatistics()->SaveTiming(name_, timer_.Elapsed(), size);
  }
}


bool CompilationPhase::ShouldProduceTraceOutput() const {
  // Trace if the appropriate trace flag is set and the phase name's first
  // character is in the FLAG_trace_phase command line parameter.
  AllowHandleDereference allow_deref;
  bool tracing_on = info()->IsStub()
      ? FLAG_trace_hydrogen_stubs
      : (FLAG_trace_hydrogen &&
         info()->closure()->PassesFilter(FLAG_trace_hydrogen_filter));
  return (tracing_on &&
      base::OS::StrChr(const_cast<char*>(FLAG_trace_phase), name_[0]) != NULL);
}

#if DEBUG
void CompilationInfo::PrintAstForTesting() {
  PrintF("--- Source from AST ---\n%s\n",
         PrettyPrinter(isolate(), zone()).PrintProgram(literal()));
}
#endif
}  // namespace internal
}  // namespace v8
