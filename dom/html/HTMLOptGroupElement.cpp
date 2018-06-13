/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/EventDispatcher.h"
#include "mozilla/EventStates.h"
#include "mozilla/dom/HTMLOptGroupElement.h"
#include "mozilla/dom/HTMLOptGroupElementBinding.h"
#include "mozilla/dom/HTMLSelectElement.h" // SafeOptionListMutation
#include "nsGkAtoms.h"
#include "nsStyleConsts.h"
#include "nsIFrame.h"
#include "nsIFormControlFrame.h"

NS_IMPL_NS_NEW_HTML_ELEMENT(OptGroup)

namespace mozilla {
namespace dom {

/**
 * The implementation of &lt;optgroup&gt;
 */



HTMLOptGroupElement::HTMLOptGroupElement(already_AddRefed<mozilla::dom::NodeInfo>& aNodeInfo)
  : nsGenericHTMLElement(aNodeInfo)
{
  // We start off enabled
  AddStatesSilently(NS_EVENT_STATE_ENABLED);
}

HTMLOptGroupElement::~HTMLOptGroupElement()
{
}


NS_IMPL_ISUPPORTS_INHERITED0(HTMLOptGroupElement, nsGenericHTMLElement)

NS_IMPL_ELEMENT_CLONE(HTMLOptGroupElement)


nsresult
HTMLOptGroupElement::GetEventTargetParent(EventChainPreVisitor& aVisitor)
{
  aVisitor.mCanHandle = false;
  // Do not process any DOM events if the element is disabled
  // XXXsmaug This is not the right thing to do. But what is?
  if (HasAttr(kNameSpaceID_None, nsGkAtoms::disabled)) {
    return NS_OK;
  }

  nsIFrame* frame = GetPrimaryFrame();
  if (frame) {
    const nsStyleUserInterface* uiStyle = frame->StyleUserInterface();
    if (uiStyle->mUserInput == StyleUserInput::None ||
        uiStyle->mUserInput == StyleUserInput::Disabled) {
      return NS_OK;
    }
  }

  return nsGenericHTMLElement::GetEventTargetParent(aVisitor);
}

Element*
HTMLOptGroupElement::GetSelect()
{
  Element* parent = nsINode::GetParentElement();
  if (!parent || !parent->IsHTMLElement(nsGkAtoms::select)) {
    return nullptr;
  }
  return parent;
}

nsresult
HTMLOptGroupElement::InsertChildAt(nsIContent* aKid,
                                   uint32_t aIndex,
                                   bool aNotify)
{
  SafeOptionListMutation safeMutation(GetSelect(), this, aKid, aIndex, aNotify);
  nsresult rv = nsGenericHTMLElement::InsertChildAt(aKid, aIndex, aNotify);
  if (NS_FAILED(rv)) {
    safeMutation.MutationFailed();
  }
  return rv;
}

void
HTMLOptGroupElement::RemoveChildAt_Deprecated(uint32_t aIndex, bool aNotify)
{
  SafeOptionListMutation safeMutation(GetSelect(), this, nullptr, aIndex,
                                      aNotify);
  nsGenericHTMLElement::RemoveChildAt_Deprecated(aIndex, aNotify);
}

void
HTMLOptGroupElement::RemoveChildNode(nsIContent* aKid, bool aNotify)
{
  SafeOptionListMutation safeMutation(GetSelect(), this, nullptr, IndexOf(aKid),
                                      aNotify);
  nsGenericHTMLElement::RemoveChildNode(aKid, aNotify);
}

nsresult
HTMLOptGroupElement::AfterSetAttr(int32_t aNameSpaceID, nsAtom* aName,
                                  const nsAttrValue* aValue,
                                  const nsAttrValue* aOldValue,
                                  nsIPrincipal* aSubjectPrincipal,
                                  bool aNotify)
{
  if (aNameSpaceID == kNameSpaceID_None && aName == nsGkAtoms::disabled) {

    EventStates disabledStates;
    if (aValue) {
      disabledStates |= NS_EVENT_STATE_DISABLED;
    } else {
      disabledStates |= NS_EVENT_STATE_ENABLED;
    }

    EventStates oldDisabledStates = State() & DISABLED_STATES;
    EventStates changedStates = disabledStates ^ oldDisabledStates;

    if (!changedStates.IsEmpty()) {
      ToggleStates(changedStates, aNotify);

      // All our children <option> have their :disabled state depending on our
      // disabled attribute. We should make sure their state is updated.
      for (nsIContent* child = nsINode::GetFirstChild(); child;
           child = child->GetNextSibling()) {
        if (auto optElement = HTMLOptionElement::FromContent(child)) {
          optElement->OptGroupDisabledChanged(true);
        }
      }
    }
  }

  return nsGenericHTMLElement::AfterSetAttr(aNameSpaceID, aName, aValue,
                                            aOldValue, aSubjectPrincipal, aNotify);
}

JSObject*
HTMLOptGroupElement::WrapNode(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return HTMLOptGroupElementBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla