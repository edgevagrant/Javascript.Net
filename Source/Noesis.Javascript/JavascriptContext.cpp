////////////////////////////////////////////////////////////////////////////////////////////////////
// File: JavascriptContext.cpp
// 
// Copyright 2010 Noesis Innovation Inc. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////

#include <msclr\lock.h>
#include <vcclr.h>
#include <msclr\marshal.h>

#include "JavascriptContext.h"

#include "SystemInterop.h"
#include "JavascriptException.h"
#include "JavascriptExternal.h"
#include "JavascriptInterop.h"

using namespace msclr;

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Noesis { namespace Javascript {

////////////////////////////////////////////////////////////////////////////////////////////////////

static DWORD curThreadId;

// Static function so it can be called from unmanaged code.
void FatalErrorCallback(const char* location, const char* message)
{
	JavascriptContext::FatalErrorCallbackMember(location, message);
}

void JavascriptContext::FatalErrorCallbackMember(const char* location, const char* message)
{
	GetCurrent()->FatalError(gcnew System::String(location), gcnew System::String(message));
}

JavascriptContext::JavascriptContext()
{
	isolate = v8::Isolate::New();
	v8::Locker v8ThreadLock(isolate);
	v8::Isolate::Scope isolate_scope(isolate);

	// If we don't call this then v8 signals an abort by triggering
	// a segmentation fault.  We want it just to cleanly shut down
	// the process, since it cannot recover from the problem.
	// It will call the FatalErrorHandler first.
	char *flags = "--no-hard_abort";
	V8::SetFlagsFromString(flags, strlen(flags));

	V8::SetFatalErrorHandler(FatalErrorCallback);

	mExternals = gcnew System::Collections::Generic::Dictionary<System::Object ^, WrappedJavascriptExternal>();
	HandleScope scope(isolate);
	mContext = new Persistent<Context>(isolate, Context::New(isolate));
	methodsForTypes = gcnew Dictionary<System::Type ^, System::Collections::Generic::Dictionary<System::String ^, WrappedMethod> ^>();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptContext::~JavascriptContext()
{
	{
		v8::Locker v8ThreadLock(isolate);
		v8::Isolate::Scope isolate_scope(isolate);
		for each (WrappedJavascriptExternal wrapped in mExternals->Values)
			delete wrapped.Pointer;
		for each (System::Collections::Generic::Dictionary<System::String ^, WrappedMethod> ^tm in methodsForTypes->Values)
			for each (WrappedMethod method in tm->Values)
				delete method.Pointer;
		delete mContext;
		delete mExternals;
	}
	if (isolate != NULL)
		isolate->Dispose();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void JavascriptContext::TerminateExecution()
{
	v8::V8::TerminateExecution(isolate);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool JavascriptContext::IsExecutionTerminating()
{
	return v8::V8::IsExecutionTerminating(isolate);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::SetParameter(System::String^ iName, System::Object^ iObject)
{
	SetParameter(iName, iObject, SetParameterOptions::None);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::SetParameter(System::String^ iName, System::Object^ iObject, SetParameterOptions options)
{
	pin_ptr<const wchar_t> namePtr = PtrToStringChars(iName);
	wchar_t* name = (wchar_t*) namePtr;
	JavascriptScope scope(this);
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	HandleScope handleScope(isolate);
	
	Handle<Value> value = JavascriptInterop::ConvertToV8(iObject);

	if (options != SetParameterOptions::None) {
		Handle<v8::Object> obj = value.As<v8::Object>();
		if (!obj.IsEmpty()) {
			Local<v8::External> wrap = obj->GetInternalField(0).As<v8::External>();
			if (!wrap.IsEmpty()) {
				JavascriptExternal* external = static_cast<JavascriptExternal*>(wrap->Value());
				external->SetOptions(options);
			}
		}
	}

	Local<Context>::New(isolate, *mContext)->Global()->Set(String::NewFromTwoByte(isolate, (uint16_t*)name), value);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::GetParameter(System::String^ iName)
{
	pin_ptr<const wchar_t> namePtr = PtrToStringChars(iName);
	wchar_t* name = (wchar_t*) namePtr;
	JavascriptScope scope(this);
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	HandleScope handleScope(isolate);
	
	Local<Value> value = Local<Context>::New(isolate, *mContext)->Global()->Get(String::NewFromTwoByte(isolate, (uint16_t*)name));
	return JavascriptInterop::ConvertFromV8(value);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::Run(System::String^ iScript)
{
	pin_ptr<const wchar_t> scriptPtr = PtrToStringChars(iScript);
	wchar_t* script = (wchar_t*)scriptPtr;
	JavascriptScope scope(this);
	SetStackLimit();
	HandleScope handleScope(JavascriptContext::GetCurrentIsolate());
	Local<Value> ret;
	
	Local<Script> compiledScript = CompileScript(script);

	{
		TryCatch tryCatch;
		ret = (*compiledScript)->Run();

		if (ret.IsEmpty())
			throw gcnew JavascriptException(tryCatch);
	}
	
	return JavascriptInterop::ConvertFromV8(ret);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Object^
JavascriptContext::Run(System::String^ iScript, System::String^ iScriptResourceName)
{
	pin_ptr<const wchar_t> scriptPtr = PtrToStringChars(iScript);
	wchar_t* script = (wchar_t*)scriptPtr;
	pin_ptr<const wchar_t> scriptResourceNamePtr = PtrToStringChars(iScriptResourceName);
	wchar_t* scriptResourceName = (wchar_t*)scriptResourceNamePtr;
	JavascriptScope scope(this);
	SetStackLimit();
	HandleScope handleScope(JavascriptContext::GetCurrentIsolate());
	Local<Value> ret;	

	Local<Script> compiledScript = CompileScript(script, scriptResourceName);
	
	{
		TryCatch tryCatch;
		ret = (*compiledScript)->Run();

		if (ret.IsEmpty())
			throw gcnew JavascriptException(tryCatch);
	}
	
	return JavascriptInterop::ConvertFromV8(ret);
}

////////////////////////////////////////////////////////////////////////////////////////////////////


void
JavascriptContext::SetStackLimit()
{
    // v8 Needs to have its stack limit set separately in each thread because
	// it detects stack overflows by reference to a stack pointer that it
	// calculates when it is first invoked.  We recalculate the stack pointer
	// for each thread.
	DWORD dw = GetCurrentThreadId();
	if (dw != curThreadId) {
		v8::ResourceConstraints rc;

        // Copied form v8/test/cctest/test-api.cc
        uint32_t size = 500000;
        uint32_t* limit = &size - (size / sizeof(size));
        // If the size is very large and the stack is very near the bottom of
        // memory then the calculation above may wrap around and give an address
        // that is above the (downwards-growing) stack.  In that case we return
        // a very low address.
        if (limit > &size)
            limit = reinterpret_cast<uint32_t*>(sizeof(size));
        
        rc.set_stack_limit((uint32_t *)(limit));
		v8::SetResourceConstraints(isolate, &rc);
		curThreadId = dw;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptContext^
JavascriptContext::GetCurrent()
{
	return sCurrentContext;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

v8::Isolate *
JavascriptContext::GetCurrentIsolate()
{
	return sCurrentContext->isolate;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

v8::Locker *
JavascriptContext::Enter()
{
	v8::Locker *locker = new v8::Locker(isolate);
	isolate->Enter();
	// We store the old context so that JavascriptContexts can be created and run
	// recursively.
	oldContext = sCurrentContext;
	sCurrentContext = this;
	HandleScope scope(isolate);
	Local<Context>::New(isolate, *mContext)->Enter();
	return locker;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void
JavascriptContext::Exit(v8::Locker *locker)
{
	{
		HandleScope scope(isolate);
		Local<Context>::New(isolate, *mContext)->Exit();
	}
	sCurrentContext = oldContext;
	isolate->Exit();
	delete locker;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// Exposed for the benefit of a regression test.
void
JavascriptContext::Collect()
{
    while(!v8::V8::IdleNotification()) {}; 
}

////////////////////////////////////////////////////////////////////////////////////////////////////

JavascriptExternal*
JavascriptContext::WrapObject(System::Object^ iObject)
{
	WrappedJavascriptExternal external_wrapped;
	if (mExternals->TryGetValue(iObject, external_wrapped))
	{
		// We've wrapped this guy before.
		return external_wrapped.Pointer;
	}
	else
	{
		JavascriptExternal* external = new JavascriptExternal(iObject);
		mExternals[iObject] = WrappedJavascriptExternal(external);
		return external;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

Handle<ObjectTemplate>
JavascriptContext::GetObjectWrapperTemplate()
{
	if (objectWrapperTemplate == NULL)
		objectWrapperTemplate = new Persistent<ObjectTemplate>(isolate, JavascriptInterop::NewObjectWrapperTemplate());
	return Local<ObjectTemplate>::New(isolate, *objectWrapperTemplate);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::String^ JavascriptContext::V8Version::get()
{
	return gcnew System::String(v8::V8::GetVersion());
}

////////////////////////////////////////////////////////////////////////////////////////////////////

Local<Script>
CompileScript(wchar_t const *source_code, wchar_t const *resource_name)
{
	// convert source
	v8::Isolate *isolate = JavascriptContext::GetCurrentIsolate();
	Local<String> source = String::NewFromTwoByte(isolate, (uint16_t const *)source_code);

	// compile
	{
		TryCatch tryCatch;

		Local<Script> script;
		if (resource_name == NULL)
		{
			script = Script::Compile(source);
		}
		else
		{
			Local<String> resource = String::NewFromTwoByte(isolate, (uint16_t const *)resource_name);
			script = Script::Compile(source, resource);
		}

		if (script.IsEmpty())
			throw gcnew JavascriptException(tryCatch);

		return script;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

System::Collections::Generic::Dictionary<System::String ^, WrappedMethod> ^
JavascriptContext::MethodsForType(System::Type ^type)
{
	System::Collections::Generic::Dictionary<System::String ^, WrappedMethod> ^res;
	if (!methodsForTypes->TryGetValue(type, res)) {
		res = gcnew System::Collections::Generic::Dictionary<System::String ^, WrappedMethod>();
		methodsForTypes[type] = res;
	}
	return res;
}

} } // namespace Noesis::Javascript

////////////////////////////////////////////////////////////////////////////////////////////////////
