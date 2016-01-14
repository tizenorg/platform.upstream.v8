// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/runtime-utils.h"

#include "src/arguments.h"
#include "src/char-predicates-inl.h"
#include "src/isolate-inl.h"
#include "src/json-parser.h"
#include "src/json-stringifier.h"
#include "src/objects-inl.h"
#ifdef SRUK_JSON_CACHE
#include "src/objects.h"
#include "src/objects-inl.h"
#endif

namespace v8 {
namespace internal {

#ifdef SRUK_INLINE_TUNING
static uint32_t sample_count_ = 0;
uint32_t GetSampleCount() { return sample_count_; }
#endif

RUNTIME_FUNCTION(Runtime_QuoteJSONString) {
  HandleScope scope(isolate);
  CONVERT_ARG_HANDLE_CHECKED(String, string, 0);
  DCHECK(args.length() == 1);
  Handle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, result, BasicJsonStringifier::StringifyString(isolate, string));
  return *result;
}


RUNTIME_FUNCTION(Runtime_BasicJSONStringify) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(Object, object, 0);
#ifdef SRUK_JSON_CACHE
  if (FLAG_json_stringify_cache) {
     if (JsonStringifyCacheManager::Get()->Activated()) {
       Handle<Object> result =
                      JsonStringifyCacheManager::Get()->Lookup(isolate, object);
       if (!result.is_null()) return *result;
     }
  }
#endif

  BasicJsonStringifier stringifier(isolate);
  Handle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, result,
                                     stringifier.Stringify(object));
#ifdef SRUK_JSON_CACHE
  if (FLAG_json_stringify_cache) {
     JsonStringifyCacheManager::Get()->Enter(isolate, object, result);
  }
#endif

  return *result;
}


RUNTIME_FUNCTION(Runtime_ParseJson) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(Object, object, 0);
  Handle<String> source;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, source,
                                     Object::ToString(isolate, object));
#ifdef SRUK_JSON_CACHE
  String* input = nullptr;
  if (FLAG_json_parse_cache) {
    input = *source;
    if(JsonParseCacheManager::Get()->Activated()) {
     Handle<Object> result = JsonParseCacheManager::Get()->Lookup(
                             isolate, source);
     if (!result.is_null()) return *result;
    }
  }
#endif

  source = String::Flatten(source);
  // Optimized fast case where we only have Latin1 characters.
  Handle<Object> result;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, result,
                                     source->IsSeqOneByteString()
                                         ? JsonParser<true>::Parse(source)
                                         : JsonParser<false>::Parse(source));
#ifdef SRUK_JSON_CACHE
  if (FLAG_json_parse_cache) {
     JsonParseCacheManager::Get()->Enter(isolate, input, result);
  }
#endif
  return *result;
}

RUNTIME_FUNCTION(Runtime_EnterSimpleLoop) {
#ifdef SRUK_JSON_CACHE
  FLAG_json_simple_loop = true;
#endif
#ifdef SRUK_INLINE_TUNING
  sample_count_++;
#endif
  return Smi::FromInt(0);
}


RUNTIME_FUNCTION(Runtime_ExitSimpleLoop) {
#ifdef SRUK_JSON_CACHE
  HandleScope scope(isolate);
  static uint32_t cnt = 0;
  FLAG_json_simple_loop = false;
  JsonParseCacheManager::Get()->Clear(isolate);
  JsonStringifyCacheManager::Get()->Clear(isolate);
  if (cnt++ > 4) FLAG_json_compiler_hint = false;
#endif
#ifdef SRUK_INLINE_TUNING
  sample_count_++;
#endif
  return Smi::FromInt(0);
}

#ifdef SRUK_JSON_CACHE
JsonParseCacheManager* JsonParseCacheManager::mgr_ = NULL;

Handle<JSObject> JsonParseCacheManager::Lookup(Isolate* isolate,
                                               Handle<String> hString) {
  if (!activated_) return Handle<JSObject>::null();
  if (*hString == string_
      && hString->map() == string_map_
      && hString->length() == length_
      && isolate->context() == context_) {
    Handle<JSObject> hObject = JsonParseCache::Lookup(isolate);
    if (hObject->IsJSObject() && HeapObject::cast(*hObject)->map() == map_) {
      return hObject;
    }
  }
  count_ = 0;
  ready_ = false;
  activated_ = false;
  JsonParseCache::Clear(isolate);
  return Handle<JSObject>::null();
}


void JsonParseCacheManager::Enter(Isolate* isolate, String* string,
                                  Handle<Object> hObject) {
  int32_t length = string->length();
  if (length < kSourceThreshold || !hObject->IsJSObject()) {
    return;
  }
  if (string != string_ || isolate->context() != context_) {
    context_ = isolate->context();
    string_ = string;
    activated_ = false;
    ready_ = false;
    count_ = 0;
  } else {
    count_++;
  }

  if (count_ == kCountThreshold) {
    JsonParseCache::Clear(isolate);
    map_ = HeapObject::cast(*hObject)->map();
    string_map_ = string->map();
    length_ = length;
  } else if (count_ > kCountThreshold){
    FLAG_json_compiler_hint = true;
    ready_ = true;
  }
  if (!ready_) return;
  if (HeapObject::cast(*hObject)->map() == map_
    && string->map() == string_map_ && length == length_
    && FLAG_json_simple_loop) {
    JsonParseCache::Enter(isolate, hObject);
    activated_ = true;
  } else {
    count_ = 0;
    ready_ = false;
    activated_ = false;
    JsonParseCache::Clear(isolate);
  }
}


JsonStringifyCacheManager* JsonStringifyCacheManager::mgr_ = NULL;

Handle<String> JsonStringifyCacheManager::Lookup(Isolate* isolate,
                                                 Handle<Object> hObject) {
  if (!activated_) return Handle<String>::null();
  if (hObject->IsJSObject()
      && *hObject == object_
      && HeapObject::cast(*hObject)->map() == object_map_
      && isolate->context() == context_) {
    Handle<String> hRes = JsonStringifyCache::Lookup(isolate);
    if (hRes->IsString() && String::cast(*hRes)->length() == length_
      && String::cast(*hRes)->map() == string_map_) {
      return hRes;
    }
  }
  count_ = 0;
  ready_ = false;
  activated_ = false;
  JsonStringifyCache::Clear(isolate);
  return Handle<String>::null();
}


void JsonStringifyCacheManager::Enter(Isolate* isolate, Handle<Object> hObject,
                                      Handle<Object> hResult) {
  if (!hObject->IsJSObject() || !hResult->IsString()) return;
  int32_t length = String::cast(*hResult)->length();
  if (length < kSourceThreshold) return;

  if (*hObject != object_ || isolate->context() != context_) {
    context_ = isolate->context();
    object_ = *hObject;
    activated_ = false;
    ready_ = false;
    count_ = 0;
  } else {
    count_++;
  }

  if (count_ == kCountThreshold) {
    JsonStringifyCache::Clear(isolate);
    object_map_ = HeapObject::cast(*hObject)->map();
    length_ = length;
    string_map_ = String::cast(*hResult)->map();
  } else if (count_ > kCountThreshold) {
    FLAG_json_compiler_hint = true;
    ready_ = true;
  }

  if (!ready_) return;

  if (HeapObject::cast(*hObject)->map() == object_map_
      && length == length_ && FLAG_json_simple_loop
      && String::cast(*hResult)->map() == string_map_) {
    JsonStringifyCache::Enter(isolate, hResult);
    activated_ = true;
  } else {
    count_ = 0;
    ready_ = false;
    activated_ = false;
    JsonStringifyCache::Clear(isolate);
  }
}
#endif

}
}  // namespace v8::internal
