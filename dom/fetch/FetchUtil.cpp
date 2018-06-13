/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FetchUtil.h"

#include "nsError.h"
#include "nsString.h"
#include "nsIDocument.h"

#include "mozilla/dom/InternalRequest.h"

namespace mozilla {
namespace dom {

// static
nsresult
FetchUtil::GetValidRequestMethod(const nsACString& aMethod, nsCString& outMethod)
{
  nsAutoCString upperCaseMethod(aMethod);
  ToUpperCase(upperCaseMethod);
  if (!NS_IsValidHTTPToken(aMethod)) {
    outMethod.SetIsVoid(true);
    return NS_ERROR_DOM_SYNTAX_ERR;
  }

  if (upperCaseMethod.EqualsLiteral("CONNECT") ||
      upperCaseMethod.EqualsLiteral("TRACE") ||
      upperCaseMethod.EqualsLiteral("TRACK")) {
    outMethod.SetIsVoid(true);
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  if (upperCaseMethod.EqualsLiteral("DELETE") ||
      upperCaseMethod.EqualsLiteral("GET") ||
      upperCaseMethod.EqualsLiteral("HEAD") ||
      upperCaseMethod.EqualsLiteral("OPTIONS") ||
      upperCaseMethod.EqualsLiteral("POST") ||
      upperCaseMethod.EqualsLiteral("PUT")) {
    outMethod = upperCaseMethod;
  }
  else {
    outMethod = aMethod; // Case unchanged for non-standard methods
  }
  return NS_OK;
}

static bool
FindCRLF(nsACString::const_iterator& aStart,
         nsACString::const_iterator& aEnd)
{
  nsACString::const_iterator end(aEnd);
  return FindInReadable(NS_LITERAL_CSTRING("\r\n"), aStart, end);
}

// Reads over a CRLF and positions start after it.
static bool
PushOverLine(nsACString::const_iterator& aStart,
	     const nsACString::const_iterator& aEnd)
{
  if (*aStart == nsCRT::CR && (aEnd - aStart > 1) && *(++aStart) == nsCRT::LF) {
    ++aStart; // advance to after CRLF
    return true;
  }

  return false;
}

// static
bool
FetchUtil::ExtractHeader(nsACString::const_iterator& aStart,
                         nsACString::const_iterator& aEnd,
                         nsCString& aHeaderName,
                         nsCString& aHeaderValue,
                         bool* aWasEmptyHeader)
{
  MOZ_ASSERT(aWasEmptyHeader);
  // Set it to a valid value here so we don't forget later.
  *aWasEmptyHeader = false;

  const char* beginning = aStart.get();
  nsACString::const_iterator end(aEnd);
  if (!FindCRLF(aStart, end)) {
    return false;
  }

  if (aStart.get() == beginning) {
    *aWasEmptyHeader = true;
    return true;
  }

  nsAutoCString header(beginning, aStart.get() - beginning);

  nsACString::const_iterator headerStart, iter, headerEnd;
  header.BeginReading(headerStart);
  header.EndReading(headerEnd);
  iter = headerStart;
  if (!FindCharInReadable(':', iter, headerEnd)) {
    return false;
  }

  aHeaderName.Assign(StringHead(header, iter - headerStart));
  aHeaderName.CompressWhitespace();
  if (!NS_IsValidHTTPToken(aHeaderName)) {
    return false;
  }

  aHeaderValue.Assign(Substring(++iter, headerEnd));
  if (!NS_IsReasonableHTTPHeaderValue(aHeaderValue)) {
    return false;
  }
  aHeaderValue.CompressWhitespace();

  return PushOverLine(aStart, aEnd);
}

// static
nsresult
FetchUtil::SetRequestReferrer(nsIPrincipal* aPrincipal,
                              nsIDocument* aDoc,
                              nsIHttpChannel* aChannel,
                              InternalRequest* aRequest) {
  MOZ_ASSERT(NS_IsMainThread());

  nsAutoString referrer;
  aRequest->GetReferrer(referrer);
  net::ReferrerPolicy policy = aRequest->GetReferrerPolicy();

  nsresult rv = NS_OK;
  if (referrer.IsEmpty()) {
    // This is the case request’s referrer is "no-referrer"
    rv = aChannel->SetReferrerWithPolicy(nullptr, net::RP_No_Referrer);
    NS_ENSURE_SUCCESS(rv, rv);
  } else if (referrer.EqualsLiteral(kFETCH_CLIENT_REFERRER_STR)) {
    rv = nsContentUtils::SetFetchReferrerURIWithPolicy(aPrincipal,
                                                       aDoc,
                                                       aChannel,
                                                       policy);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    // From "Determine request's Referrer" step 3
    // "If request's referrer is a URL, let referrerSource be request's
    // referrer."
    nsCOMPtr<nsIURI> referrerURI;
    rv = NS_NewURI(getter_AddRefs(referrerURI), referrer, nullptr, nullptr);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aChannel->SetReferrerWithPolicy(referrerURI, policy);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIURI> referrerURI;
  Unused << aChannel->GetReferrer(getter_AddRefs(referrerURI));

  // Step 8 https://fetch.spec.whatwg.org/#main-fetch
  // If request’s referrer is not "no-referrer", set request’s referrer to
  // the result of invoking determine request’s referrer.
  if (referrerURI) {
    nsAutoCString spec;
    rv = referrerURI->GetSpec(spec);
    NS_ENSURE_SUCCESS(rv, rv);

    aRequest->SetReferrer(NS_ConvertUTF8toUTF16(spec));
  } else {
    aRequest->SetReferrer(EmptyString());
  }

  return NS_OK;
}

class WindowStreamOwner final : public nsIObserver
                              , public nsSupportsWeakReference
{
  // Read from any thread but only set/cleared on the main thread. The lifecycle
  // of WindowStreamOwner prevents concurrent read/clear.
  nsCOMPtr<nsIAsyncInputStream> mStream;

  nsCOMPtr<nsIGlobalObject> mGlobal;

  ~WindowStreamOwner()
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
    if (obs) {
      obs->RemoveObserver(this, DOM_WINDOW_DESTROYED_TOPIC);
    }
  }

public:
  NS_DECL_ISUPPORTS

  WindowStreamOwner(nsIAsyncInputStream* aStream, nsIGlobalObject* aGlobal)
    : mStream(aStream)
    , mGlobal(aGlobal)
  {
    MOZ_DIAGNOSTIC_ASSERT(mGlobal);
    MOZ_ASSERT(NS_IsMainThread());
  }

  static already_AddRefed<WindowStreamOwner>
  Create(nsIAsyncInputStream* aStream, nsIGlobalObject* aGlobal)
  {
    nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
    if (NS_WARN_IF(!os)) {
      return nullptr;
    }

    RefPtr<WindowStreamOwner> self = new WindowStreamOwner(aStream, aGlobal);

    // Holds nsIWeakReference to self.
    nsresult rv = os->AddObserver(self, DOM_WINDOW_DESTROYED_TOPIC, true);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return nullptr;
    }

    return self.forget();
  }

  struct Destroyer final : Runnable
  {
    RefPtr<WindowStreamOwner> mDoomed;

    explicit Destroyer(already_AddRefed<WindowStreamOwner> aDoomed)
      : Runnable("WindowStreamOwner::Destroyer")
      , mDoomed(aDoomed)
    {}

    NS_IMETHOD
    Run() override
    {
      mDoomed = nullptr;
      return NS_OK;
    }
  };

  // nsIObserver:

  NS_IMETHOD
  Observe(nsISupports* aSubject, const char* aTopic, const char16_t* aData) override
  {
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_DIAGNOSTIC_ASSERT(strcmp(aTopic, DOM_WINDOW_DESTROYED_TOPIC) == 0);

    if (!mStream) {
      return NS_OK;
    }

    nsCOMPtr<nsPIDOMWindowInner> window = do_QueryInterface(mGlobal);
    if (!SameCOMIdentity(aSubject, window)) {
      return NS_OK;
    }

    // mStream->Close() will call JSStreamConsumer::OnInputStreamReady which may
    // then destory itself, dropping the last reference to 'this'.
    RefPtr<WindowStreamOwner> keepAlive(this);

    mStream->Close();
    mStream = nullptr;
    mGlobal = nullptr;
    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS(WindowStreamOwner, nsIObserver, nsISupportsWeakReference)

class WorkerStreamOwner final : public WorkerHolder
{
  // Read from any thread but only set/cleared on the worker thread. The
  // lifecycle of WorkerStreamOwner prevents concurrent read/clear.
  nsCOMPtr<nsIAsyncInputStream> mStream;

public:
  explicit WorkerStreamOwner(nsIAsyncInputStream* aStream)
    : WorkerHolder("WorkerStreamOwner",
                   WorkerHolder::Behavior::AllowIdleShutdownStart)
    , mStream(aStream)
  {}

  static UniquePtr<WorkerStreamOwner>
  Create(nsIAsyncInputStream* aStream, WorkerPrivate* aWorker)
  {
    auto self = MakeUnique<WorkerStreamOwner>(aStream);

    if (!self->HoldWorker(aWorker, workers::Closing)) {
      return nullptr;
    }

    return self;
  }

  struct Destroyer final : CancelableRunnable
  {
    UniquePtr<WorkerStreamOwner> mDoomed;

    explicit Destroyer(UniquePtr<WorkerStreamOwner>&& aDoomed)
      : CancelableRunnable("WorkerStreamOwner::Destroyer")
      , mDoomed(Move(aDoomed))
    {}

    NS_IMETHOD
    Run() override
    {
      mDoomed = nullptr;
      return NS_OK;
    }

    nsresult
    Cancel() override
    {
      return Run();
    }
  };

  // WorkerHolder:

  bool Notify(workers::Status aStatus) override
  {
    if (!mStream) {
      return true;
    }

    // If this Close() calls JSStreamConsumer::OnInputStreamReady and drops the
    // last reference to the JSStreamConsumer, 'this' will not be destroyed
    // since ~JSStreamConsumer() only enqueues a Destroyer.
    mStream->Close();
    mStream = nullptr;
    ReleaseWorker();
    return true;
  }
};

class JSStreamConsumer final : public nsIInputStreamCallback
{
  nsCOMPtr<nsIEventTarget> mOwningEventTarget;
  RefPtr<WindowStreamOwner> mWindowStreamOwner;
  UniquePtr<WorkerStreamOwner> mWorkerStreamOwner;
  JS::StreamConsumer* mConsumer;
  bool mConsumerAborted;

  JSStreamConsumer(already_AddRefed<WindowStreamOwner> aWindowStreamOwner,
                   nsIGlobalObject* aGlobal,
                   JS::StreamConsumer* aConsumer)
   : mOwningEventTarget(aGlobal->EventTargetFor(TaskCategory::Other))
   , mWindowStreamOwner(aWindowStreamOwner)
   , mConsumer(aConsumer)
   , mConsumerAborted(false)
  {
    MOZ_DIAGNOSTIC_ASSERT(mWindowStreamOwner);
    MOZ_DIAGNOSTIC_ASSERT(mConsumer);
  }

  JSStreamConsumer(UniquePtr<WorkerStreamOwner> aWorkerStreamOwner,
                   nsIGlobalObject* aGlobal,
                   JS::StreamConsumer* aConsumer)
   : mOwningEventTarget(aGlobal->EventTargetFor(TaskCategory::Other))
   , mWorkerStreamOwner(Move(aWorkerStreamOwner))
   , mConsumer(aConsumer)
   , mConsumerAborted(false)
  {
    MOZ_DIAGNOSTIC_ASSERT(mWorkerStreamOwner);
    MOZ_DIAGNOSTIC_ASSERT(mConsumer);
  }

  ~JSStreamConsumer()
  {
    // Both WindowStreamOwner and WorkerStreamOwner need to be destroyed on
    // their global's event target thread.

    RefPtr<Runnable> destroyer;
    if (mWindowStreamOwner) {
      MOZ_DIAGNOSTIC_ASSERT(!mWorkerStreamOwner);
      destroyer = new WindowStreamOwner::Destroyer(mWindowStreamOwner.forget());
    } else {
      MOZ_DIAGNOSTIC_ASSERT(mWorkerStreamOwner);
      destroyer = new WorkerStreamOwner::Destroyer(Move(mWorkerStreamOwner));
    }

    MOZ_ALWAYS_SUCCEEDS(mOwningEventTarget->Dispatch(destroyer.forget()));
  }

  static nsresult WriteSegment(nsIInputStream* aStream,
                               void* aClosure,
                               const char* aFromSegment,
                               uint32_t aToOffset,
                               uint32_t aCount,
                               uint32_t* aWriteCount)
  {
    JSStreamConsumer* self = reinterpret_cast<JSStreamConsumer*>(aClosure);
    MOZ_DIAGNOSTIC_ASSERT(!self->mConsumerAborted);

    // This callback can be called on any thread which is explicitly allowed by
    // this particular JS API call.
    if (!self->mConsumer->consumeChunk((const uint8_t*)aFromSegment, aCount)) {
      self->mConsumerAborted = true;
      return NS_ERROR_UNEXPECTED;
    }

    *aWriteCount = aCount;
    return NS_OK;
  }

public:
  NS_DECL_THREADSAFE_ISUPPORTS

  static bool Start(nsIInputStream* aStream,
                    JS::StreamConsumer* aConsumer,
                    nsIGlobalObject* aGlobal,
                    workers::WorkerPrivate* aMaybeWorker)
  {
    nsresult rv;

    bool nonBlocking = false;
    rv = aStream->IsNonBlocking(&nonBlocking);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }

    // Use a pipe to create an nsIAsyncInputStream if we don't already have one.
    nsCOMPtr<nsIAsyncInputStream> asyncStream = do_QueryInterface(aStream);
    if (!asyncStream || !nonBlocking) {
      nsCOMPtr<nsIAsyncOutputStream> pipe;
      rv = NS_NewPipe2(getter_AddRefs(asyncStream), getter_AddRefs(pipe),
                       true, true);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return false;
      }

      nsCOMPtr<nsIEventTarget> thread =
        do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);

      rv = NS_AsyncCopy(aStream, pipe, thread,
                        NS_ASYNCCOPY_VIA_WRITESEGMENTS);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return false;
      }
    }

    RefPtr<JSStreamConsumer> consumer;
    if (aMaybeWorker) {
      UniquePtr<WorkerStreamOwner> owner =
        WorkerStreamOwner::Create(asyncStream, aMaybeWorker);
      if (!owner) {
        return false;
      }

      consumer = new JSStreamConsumer(Move(owner), aGlobal, aConsumer);
    } else {
      RefPtr<WindowStreamOwner> owner =
        WindowStreamOwner::Create(asyncStream, aGlobal);
      if (!owner) {
        return false;
      }

      consumer = new JSStreamConsumer(owner.forget(), aGlobal, aConsumer);
    }

    // This AsyncWait() creates a ref-cycle between asyncStream and consumer:
    //
    //   asyncStream -> consumer -> (Window|Worker)StreamOwner -> asyncStream
    //
    // The cycle is broken when the stream completes or errors out and
    // asyncStream drops its reference to consumer.
    return NS_SUCCEEDED(asyncStream->AsyncWait(consumer, 0, 0, nullptr));
  }

  // nsIInputStreamCallback:

  NS_IMETHOD
  OnInputStreamReady(nsIAsyncInputStream* aStream) override
  {
    // Can be called on any stream. The JS API calls made below explicitly
    // support being called from any thread.
    MOZ_DIAGNOSTIC_ASSERT(!mConsumerAborted);

    nsresult rv;

    uint64_t available = 0;
    rv = aStream->Available(&available);
    if (NS_SUCCEEDED(rv) && available == 0) {
      rv = NS_BASE_STREAM_CLOSED;
    }

    if (rv == NS_BASE_STREAM_CLOSED) {
      mConsumer->streamClosed(JS::StreamConsumer::EndOfFile);
      return NS_OK;
    }

    if (NS_FAILED(rv)) {
      mConsumer->streamClosed(JS::StreamConsumer::Error);
      return NS_OK;
    }

    // Check mConsumerAborted before NS_FAILED to avoid calling streamClosed()
    // if consumeChunk() returned false per JS API contract.
    uint32_t written = 0;
    rv = aStream->ReadSegments(WriteSegment, this, available, &written);
    if (mConsumerAborted) {
      return NS_OK;
    }
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mConsumer->streamClosed(JS::StreamConsumer::Error);
      return NS_OK;
    }

    rv = aStream->AsyncWait(this, 0, 0, nullptr);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mConsumer->streamClosed(JS::StreamConsumer::Error);
      return NS_OK;
    }

    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS(JSStreamConsumer,
                  nsIInputStreamCallback)

static bool
ThrowException(JSContext* aCx, unsigned errorNumber)
{
  JS_ReportErrorNumberASCII(aCx, js::GetErrorMessage, nullptr, errorNumber);
  return false;
}

// static
bool
FetchUtil::StreamResponseToJS(JSContext* aCx,
                              JS::HandleObject aObj,
                              JS::MimeType aMimeType,
                              JS::StreamConsumer* aConsumer,
                              workers::WorkerPrivate* aMaybeWorker)
{
  MOZ_ASSERT(!aMaybeWorker == NS_IsMainThread());

  RefPtr<Response> response;
  nsresult rv = UNWRAP_OBJECT(Response, aObj, response);
  if (NS_FAILED(rv)) {
    return ThrowException(aCx, JSMSG_BAD_RESPONSE_VALUE);
  }

  const char* requiredMimeType = nullptr;
  switch (aMimeType) {
    case JS::MimeType::Wasm:
      requiredMimeType = "application/wasm";
      break;
  }

  if (strcmp(requiredMimeType, response->MimeType().Data())) {
    return ThrowException(aCx, JSMSG_BAD_RESPONSE_MIME_TYPE);
  }

  if (response->Type() != ResponseType::Basic &&
      response->Type() != ResponseType::Cors &&
      response->Type() != ResponseType::Default) {
    return ThrowException(aCx, JSMSG_BAD_RESPONSE_CORS_SAME_ORIGIN);
  }

  if (!response->Ok()) {
    return ThrowException(aCx, JSMSG_BAD_RESPONSE_STATUS);
  }

  if (response->BodyUsed()) {
    return ThrowException(aCx, JSMSG_RESPONSE_ALREADY_CONSUMED);
  }

  RefPtr<InternalResponse> ir = response->GetInternalResponse();
  if (NS_WARN_IF(!ir)) {
    return ThrowException(aCx, JSMSG_OUT_OF_MEMORY);
  }

  nsCOMPtr<nsIInputStream> body;
  ir->GetUnfilteredBody(getter_AddRefs(body));
  if (!body) {
    aConsumer->streamClosed(JS::StreamConsumer::EndOfFile);
    return true;
  }

  IgnoredErrorResult error;
  response->SetBodyUsed(aCx, error);
  if (NS_WARN_IF(error.Failed())) {
    return ThrowException(aCx, JSMSG_ERROR_CONSUMING_RESPONSE);
  }

  nsIGlobalObject* global = xpc::NativeGlobal(aObj);

  if (!JSStreamConsumer::Start(body, aConsumer, global, aMaybeWorker)) {
    return ThrowException(aCx, JSMSG_OUT_OF_MEMORY);
  }

  return true;
}

} // namespace dom
} // namespace mozilla