/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerRegistration.h"

#include "mozilla/dom/DOMMozPromiseRequestHolder.h"
#include "mozilla/dom/Notification.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PushManager.h"
#include "mozilla/dom/ServiceWorker.h"
#include "mozilla/dom/ServiceWorkerRegistrationBinding.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupportsPrimitives.h"
#include "nsPIDOMWindow.h"
#include "ServiceWorkerRegistrationImpl.h"

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(ServiceWorkerRegistration,
                                   DOMEventTargetHelper,
                                   mInstallingWorker,
                                   mWaitingWorker,
                                   mActiveWorker,
                                   mPushManager);

NS_IMPL_ADDREF_INHERITED(ServiceWorkerRegistration, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(ServiceWorkerRegistration, DOMEventTargetHelper)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ServiceWorkerRegistration)
  NS_INTERFACE_MAP_ENTRY(ServiceWorkerRegistration)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

ServiceWorkerRegistration::ServiceWorkerRegistration(nsIGlobalObject* aGlobal,
                                                     const ServiceWorkerRegistrationDescriptor& aDescriptor,
                                                     ServiceWorkerRegistration::Inner* aInner)
  : DOMEventTargetHelper(aGlobal)
  , mDescriptor(aDescriptor)
  , mInner(aInner)
{
  MOZ_DIAGNOSTIC_ASSERT(mInner);

  KeepAliveIfHasListenersFor(NS_LITERAL_STRING("updatefound"));

  UpdateState(mDescriptor);
  mInner->SetServiceWorkerRegistration(this);
}

ServiceWorkerRegistration::~ServiceWorkerRegistration()
{
  mInner->ClearServiceWorkerRegistration(this);
}

JSObject*
ServiceWorkerRegistration::WrapObject(JSContext* aCx,
                                      JS::Handle<JSObject*> aGivenProto)
{
  return ServiceWorkerRegistration_Binding::Wrap(aCx, this, aGivenProto);
}

/* static */ already_AddRefed<ServiceWorkerRegistration>
ServiceWorkerRegistration::CreateForMainThread(nsPIDOMWindowInner* aWindow,
                                               const ServiceWorkerRegistrationDescriptor& aDescriptor)
{
  MOZ_ASSERT(aWindow);
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<Inner> inner = new ServiceWorkerRegistrationMainThread(aDescriptor);

  RefPtr<ServiceWorkerRegistration> registration =
    new ServiceWorkerRegistration(aWindow->AsGlobal(), aDescriptor, inner);

  return registration.forget();
}

/* static */ already_AddRefed<ServiceWorkerRegistration>
ServiceWorkerRegistration::CreateForWorker(WorkerPrivate* aWorkerPrivate,
                                           nsIGlobalObject* aGlobal,
                                           const ServiceWorkerRegistrationDescriptor& aDescriptor)
{
  MOZ_DIAGNOSTIC_ASSERT(aWorkerPrivate);
  MOZ_DIAGNOSTIC_ASSERT(aGlobal);
  aWorkerPrivate->AssertIsOnWorkerThread();

  RefPtr<Inner> inner =
    new ServiceWorkerRegistrationWorkerThread(aDescriptor);

  RefPtr<ServiceWorkerRegistration> registration =
    new ServiceWorkerRegistration(aGlobal, aDescriptor, inner);

  return registration.forget();
}

void
ServiceWorkerRegistration::DisconnectFromOwner()
{
  DOMEventTargetHelper::DisconnectFromOwner();
}

void
ServiceWorkerRegistration::RegistrationRemoved()
{
  // Our underlying registration was removed from SWM, so we
  // will never get an updatefound event again.  We can let
  // the object GC if content is not holding it alive.
  IgnoreKeepAliveIfHasListenersFor(NS_LITERAL_STRING("updatefound"));
}

already_AddRefed<ServiceWorker>
ServiceWorkerRegistration::GetInstalling() const
{
  RefPtr<ServiceWorker> ref = mInstallingWorker;
  return ref.forget();
}

already_AddRefed<ServiceWorker>
ServiceWorkerRegistration::GetWaiting() const
{
  RefPtr<ServiceWorker> ref = mWaitingWorker;
  return ref.forget();
}

already_AddRefed<ServiceWorker>
ServiceWorkerRegistration::GetActive() const
{
  RefPtr<ServiceWorker> ref = mActiveWorker;
  return ref.forget();
}

void
ServiceWorkerRegistration::UpdateState(const ServiceWorkerRegistrationDescriptor& aDescriptor)
{
  MOZ_DIAGNOSTIC_ASSERT(MatchesDescriptor(aDescriptor));

  mDescriptor = aDescriptor;

  nsCOMPtr<nsIGlobalObject> global = GetParentObject();

  // Move the currently exposed workers into a separate list
  // of "old" workers.  We will then potentially add them
  // back to the registration properties below based on the
  // given descriptor.  Any that are not restored will need
  // to be moved to the redundant state.
  AutoTArray<RefPtr<ServiceWorker>, 3> oldWorkerList({
    mInstallingWorker.forget(),
    mWaitingWorker.forget(),
    mActiveWorker.forget(),
  });

  // Its important that all state changes are actually applied before
  // dispatching any statechange events.  Each ServiceWorker object
  // should be in the correct state and the ServiceWorkerRegistration
  // properties need to be set correctly as well.  To accomplish this
  // we use a ScopeExit to dispatch any statechange events.
  auto scopeExit = MakeScopeExit([&] {
    // Check to see if any of the "old" workers was completely discarded.
    // Set these workers to the redundant state.
    for (auto& oldWorker : oldWorkerList) {
      if (!oldWorker ||
           oldWorker == mInstallingWorker ||
           oldWorker == mWaitingWorker ||
           oldWorker == mActiveWorker) {
        continue;
      }

      oldWorker->SetState(ServiceWorkerState::Redundant);
    }

    // Check each worker to see if it needs a statechange event dispatched.
    if (mInstallingWorker) {
      mInstallingWorker->MaybeDispatchStateChangeEvent();
    }
    if (mWaitingWorker) {
      mWaitingWorker->MaybeDispatchStateChangeEvent();
    }
    if (mActiveWorker) {
      mActiveWorker->MaybeDispatchStateChangeEvent();
    }

    // We also check the "old" workers to see if they need a statechange
    // event as well.  Note, these may overlap with the known worker properties
    // above, but MaybeDispatchStateChangeEvent() will ignore duplicated calls.
    for (auto& oldWorker : oldWorkerList) {
      if (!oldWorker) {
        continue;
      }

      oldWorker->MaybeDispatchStateChangeEvent();
    }
  });

  // Clear all workers if the registration has been detached from the global.
  // Also, we cannot expose ServiceWorker objects on worker threads yet, so
  // do the same on when off-main-thread.  This main thread check should be
  // removed as part of bug 1113522.
  if (!global || !NS_IsMainThread()) {
    return;
  }

  Maybe<ServiceWorkerDescriptor> active = aDescriptor.GetActive();
  if (active.isSome()) {
    mActiveWorker = global->GetOrCreateServiceWorker(active.ref());
    mActiveWorker->SetState(active.ref().State());
  } else {
    mActiveWorker = nullptr;
  }

  Maybe<ServiceWorkerDescriptor> waiting = aDescriptor.GetWaiting();
  if (waiting.isSome()) {
    mWaitingWorker = global->GetOrCreateServiceWorker(waiting.ref());
    mWaitingWorker->SetState(waiting.ref().State());
  } else {
    mWaitingWorker = nullptr;
  }

  Maybe<ServiceWorkerDescriptor> installing = aDescriptor.GetInstalling();
  if (installing.isSome()) {
    mInstallingWorker = global->GetOrCreateServiceWorker(installing.ref());
    mInstallingWorker->SetState(installing.ref().State());
  } else {
    mInstallingWorker = nullptr;
  }
}

bool
ServiceWorkerRegistration::MatchesDescriptor(const ServiceWorkerRegistrationDescriptor& aDescriptor) const
{
  return aDescriptor.Id() == mDescriptor.Id() &&
         aDescriptor.PrincipalInfo() == mDescriptor.PrincipalInfo() &&
         aDescriptor.Scope() == mDescriptor.Scope();
}

void
ServiceWorkerRegistration::GetScope(nsAString& aScope) const
{
  CopyUTF8toUTF16(mDescriptor.Scope(), aScope);
}

ServiceWorkerUpdateViaCache
ServiceWorkerRegistration::GetUpdateViaCache(ErrorResult& aRv) const
{
  return mDescriptor.UpdateViaCache();
}

already_AddRefed<Promise>
ServiceWorkerRegistration::Update(ErrorResult& aRv)
{
  if (!mInner) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  nsIGlobalObject* global = GetParentObject();
  if (!global) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  RefPtr<Promise> outer = Promise::Create(global, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  RefPtr<ServiceWorkerRegistration> self = this;

  mInner->Update(
    [outer, self](const ServiceWorkerRegistrationDescriptor& aDesc) {
      nsIGlobalObject* global = self->GetParentObject();
      MOZ_DIAGNOSTIC_ASSERT(global);
      RefPtr<ServiceWorkerRegistration> ref =
        global->GetOrCreateServiceWorkerRegistration(aDesc);
      if (!ref) {
        outer->MaybeReject(NS_ERROR_DOM_INVALID_STATE_ERR);
        return;
      }
      outer->MaybeResolve(ref);
    }, [outer] (ErrorResult& aRv) {
      outer->MaybeReject(aRv);
    });

  return outer.forget();
}

already_AddRefed<Promise>
ServiceWorkerRegistration::Unregister(ErrorResult& aRv)
{
  nsIGlobalObject* global = GetParentObject();
  if (NS_WARN_IF(!global)) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  RefPtr<Promise> outer = Promise::Create(global, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  if (!mInner) {
    outer->MaybeResolve(false);
    return outer.forget();
  }

  mInner->Unregister(
    [outer] (bool aSuccess) {
      outer->MaybeResolve(aSuccess);
    }, [outer] (ErrorResult& aRv) {
      // register() should be resilient and resolve false instead
      // of rejecting in most cases.
      outer->MaybeResolve(false);
    });

  return outer.forget();
}

already_AddRefed<PushManager>
ServiceWorkerRegistration::GetPushManager(JSContext* aCx, ErrorResult& aRv)
{
  if (!mPushManager) {
    nsCOMPtr<nsIGlobalObject> globalObject = GetParentObject();

    if (!globalObject) {
      aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    GlobalObject global(aCx, globalObject->GetGlobalJSObject());
    mPushManager =
      PushManager::Constructor(global,
                               NS_ConvertUTF8toUTF16(mDescriptor.Scope()),
                               aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  RefPtr<PushManager> ret = mPushManager;
  return ret.forget();
}

already_AddRefed<Promise>
ServiceWorkerRegistration::ShowNotification(JSContext* aCx,
                                            const nsAString& aTitle,
                                            const NotificationOptions& aOptions,
                                            ErrorResult& aRv)
{
  nsIGlobalObject* global = GetParentObject();
  if (!global) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  NS_ConvertUTF8toUTF16 scope(mDescriptor.Scope());

  // Until we ship ServiceWorker objects on worker threads the active
  // worker will always be nullptr.  So limit this check to main
  // thread for now.
  MOZ_ASSERT_IF(!NS_IsMainThread(), mDescriptor.GetActive().isNothing());
  if (mDescriptor.GetActive().isNothing() && NS_IsMainThread()) {
    aRv.ThrowTypeError<MSG_NO_ACTIVE_WORKER>(scope);
    return nullptr;
  }

  RefPtr<Promise> p =
    Notification::ShowPersistentNotification(aCx, global, scope,
                                             aTitle, aOptions, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  return p.forget();
}

already_AddRefed<Promise>
ServiceWorkerRegistration::GetNotifications(const GetNotificationOptions& aOptions,
                                            ErrorResult& aRv)
{
  nsIGlobalObject* global = GetParentObject();
  if (!global) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  NS_ConvertUTF8toUTF16 scope(mDescriptor.Scope());

  if (NS_IsMainThread()) {
    nsCOMPtr<nsPIDOMWindowInner> window = do_QueryInterface(global);
    if (NS_WARN_IF(!window)) {
      aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }
    return Notification::Get(window, aOptions, scope, aRv);
  }

  WorkerPrivate* worker = GetCurrentThreadWorkerPrivate();
  worker->AssertIsOnWorkerThread();
  return Notification::WorkerGet(worker, aOptions, scope, aRv);
}

const ServiceWorkerRegistrationDescriptor&
ServiceWorkerRegistration::Descriptor() const
{
  return mDescriptor;
}

} // dom namespace
} // mozilla namespace
