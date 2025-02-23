/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAnimationManager.h"
#include "nsTransitionManager.h"
#include "mozilla/dom/CSSAnimationBinding.h"

#include "mozilla/EffectCompositor.h"
#include "mozilla/EffectSet.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/StyleAnimationValue.h"
#include "mozilla/dom/DocumentTimeline.h"
#include "mozilla/dom/KeyframeEffect.h"

#include "nsPresContext.h"
#include "nsStyleSet.h"
#include "nsStyleChangeList.h"
#include "nsCSSRules.h"
#include "RestyleManager.h"
#include "nsLayoutUtils.h"
#include "nsIFrame.h"
#include "nsIDocument.h"
#include "nsDOMMutationObserver.h"
#include <math.h>

using namespace mozilla;
using namespace mozilla::css;
using mozilla::dom::Animation;
using mozilla::dom::AnimationPlayState;
using mozilla::dom::KeyframeEffectReadOnly;
using mozilla::dom::CSSAnimation;

////////////////////////// CSSAnimation ////////////////////////////

JSObject*
CSSAnimation::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return dom::CSSAnimationBinding::Wrap(aCx, this, aGivenProto);
}

mozilla::dom::Promise*
CSSAnimation::GetReady(ErrorResult& aRv)
{
  FlushStyle();
  return Animation::GetReady(aRv);
}

void
CSSAnimation::Play(ErrorResult &aRv, LimitBehavior aLimitBehavior)
{
  mPauseShouldStick = false;
  Animation::Play(aRv, aLimitBehavior);
}

void
CSSAnimation::Pause(ErrorResult& aRv)
{
  mPauseShouldStick = true;
  Animation::Pause(aRv);
}

AnimationPlayState
CSSAnimation::PlayStateFromJS() const
{
  // Flush style to ensure that any properties controlling animation state
  // (e.g. animation-play-state) are fully updated.
  FlushStyle();
  return Animation::PlayStateFromJS();
}

void
CSSAnimation::PlayFromJS(ErrorResult& aRv)
{
  // Note that flushing style below might trigger calls to
  // PlayFromStyle()/PauseFromStyle() on this object.
  FlushStyle();
  Animation::PlayFromJS(aRv);
}

void
CSSAnimation::PlayFromStyle()
{
  mIsStylePaused = false;
  if (!mPauseShouldStick) {
    ErrorResult rv;
    DoPlay(rv, Animation::LimitBehavior::Continue);
    // play() should not throw when LimitBehavior is Continue
    MOZ_ASSERT(!rv.Failed(), "Unexpected exception playing animation");
  }
}

void
CSSAnimation::PauseFromStyle()
{
  // Check if the pause state is being overridden
  if (mIsStylePaused) {
    return;
  }

  mIsStylePaused = true;
  ErrorResult rv;
  DoPause(rv);
  // pause() should only throw when *all* of the following conditions are true:
  // - we are in the idle state, and
  // - we have a negative playback rate, and
  // - we have an infinitely repeating animation
  // The first two conditions will never happen under regular style processing
  // but could happen if an author made modifications to the Animation object
  // and then updated animation-play-state. It's an unusual case and there's
  // no obvious way to pass on the exception information so we just silently
  // fail for now.
  if (rv.Failed()) {
    NS_WARNING("Unexpected exception pausing animation - silently failing");
  }
}

void
CSSAnimation::Tick()
{
  Animation::Tick();
  QueueEvents();
}

bool
CSSAnimation::HasLowerCompositeOrderThan(const CSSAnimation& aOther) const
{
  MOZ_ASSERT(IsTiedToMarkup() && aOther.IsTiedToMarkup(),
             "Should only be called for CSS animations that are sorted "
             "as CSS animations (i.e. tied to CSS markup)");

  // 0. Object-equality case
  if (&aOther == this) {
    return false;
  }

  // 1. Sort by document order
  if (!mOwningElement.Equals(aOther.mOwningElement)) {
    return mOwningElement.LessThan(aOther.mOwningElement);
  }

  // 2. (Same element and pseudo): Sort by position in animation-name
  return mAnimationIndex < aOther.mAnimationIndex;
}

void
CSSAnimation::QueueEvents()
{
  if (!mEffect) {
    return;
  }

  // CSS animations dispatch events at their owning element. This allows
  // script to repurpose a CSS animation to target a different element,
  // to use a group effect (which has no obvious "target element"), or
  // to remove the animation effect altogether whilst still getting
  // animation events.
  //
  // It does mean, however, that for a CSS animation that has no owning
  // element (e.g. it was created using the CSSAnimation constructor or
  // disassociated from CSS) no events are fired. If it becomes desirable
  // for these animations to still fire events we should spec the concept
  // of the "original owning element" or "event target" and allow script
  // to set it when creating a CSSAnimation object.
  if (!mOwningElement.IsSet()) {
    return;
  }

  dom::Element* owningElement;
  nsCSSPseudoElements::Type owningPseudoType;
  mOwningElement.GetElement(owningElement, owningPseudoType);
  MOZ_ASSERT(owningElement, "Owning element should be set");

  // Get the nsAnimationManager so we can queue events on it
  nsPresContext* presContext = mOwningElement.GetRenderedPresContext();
  if (!presContext) {
    return;
  }
  nsAnimationManager* manager = presContext->AnimationManager();

  ComputedTiming computedTiming = mEffect->GetComputedTiming();

  if (computedTiming.mPhase == ComputedTiming::AnimationPhase::Null) {
    return; // do nothing
  }

  // Note that script can change the start time, so we have to handle moving
  // backwards through the animation as well as forwards. An 'animationstart'
  // is dispatched if we enter the active phase (regardless if that is from
  // before or after the animation's active phase). An 'animationend' is
  // dispatched if we leave the active phase (regardless if that is to before
  // or after the animation's active phase).

  bool wasActive = mPreviousPhaseOrIteration != PREVIOUS_PHASE_BEFORE &&
                   mPreviousPhaseOrIteration != PREVIOUS_PHASE_AFTER;
  bool isActive =
         computedTiming.mPhase == ComputedTiming::AnimationPhase::Active;
  bool isSameIteration =
         computedTiming.mCurrentIteration == mPreviousPhaseOrIteration;
  bool skippedActivePhase =
    (mPreviousPhaseOrIteration == PREVIOUS_PHASE_BEFORE &&
     computedTiming.mPhase == ComputedTiming::AnimationPhase::After) ||
    (mPreviousPhaseOrIteration == PREVIOUS_PHASE_AFTER &&
     computedTiming.mPhase == ComputedTiming::AnimationPhase::Before);

  MOZ_ASSERT(!skippedActivePhase || (!isActive && !wasActive),
             "skippedActivePhase only makes sense if we were & are inactive");

  if (computedTiming.mPhase == ComputedTiming::AnimationPhase::Before) {
    mPreviousPhaseOrIteration = PREVIOUS_PHASE_BEFORE;
  } else if (computedTiming.mPhase == ComputedTiming::AnimationPhase::Active) {
    mPreviousPhaseOrIteration = computedTiming.mCurrentIteration;
  } else if (computedTiming.mPhase == ComputedTiming::AnimationPhase::After) {
    mPreviousPhaseOrIteration = PREVIOUS_PHASE_AFTER;
  }

  EventMessage message;

  if (!wasActive && isActive) {
    message = eAnimationStart;
  } else if (wasActive && !isActive) {
    message = eAnimationEnd;
  } else if (wasActive && isActive && !isSameIteration) {
    message = eAnimationIteration;
  } else if (skippedActivePhase) {
    // First notifying for start of 0th iteration by appending an
    // 'animationstart':
    StickyTimeDuration elapsedTime =
      std::min(StickyTimeDuration(InitialAdvance()),
               computedTiming.mActiveDuration);
    manager->QueueEvent(AnimationEventInfo(owningElement, owningPseudoType,
                                           eAnimationStart, mAnimationName,
                                           elapsedTime,
                                           ElapsedTimeToTimeStamp(elapsedTime),
                                           this));
    // Then have the shared code below append an 'animationend':
    message = eAnimationEnd;
  } else {
    return; // No events need to be sent
  }

  StickyTimeDuration elapsedTime;

  if (message == eAnimationStart || message == eAnimationIteration) {
    StickyTimeDuration iterationStart = computedTiming.mDuration *
                                          computedTiming.mCurrentIteration;
    elapsedTime = std::max(iterationStart, StickyTimeDuration(InitialAdvance()));
  } else {
    MOZ_ASSERT(message == eAnimationEnd);
    elapsedTime = computedTiming.mActiveDuration;
  }

  manager->QueueEvent(AnimationEventInfo(owningElement, owningPseudoType,
                                         message, mAnimationName, elapsedTime,
                                         ElapsedTimeToTimeStamp(elapsedTime),
                                         this));
}

void
CSSAnimation::UpdateTiming(SeekFlag aSeekFlag, SyncNotifyFlag aSyncNotifyFlag)
{
  if (mNeedsNewAnimationIndexWhenRun &&
      PlayState() != AnimationPlayState::Idle) {
    mAnimationIndex = sNextAnimationIndex++;
    mNeedsNewAnimationIndexWhenRun = false;
  }

  Animation::UpdateTiming(aSeekFlag, aSyncNotifyFlag);
}

TimeStamp
CSSAnimation::ElapsedTimeToTimeStamp(const StickyTimeDuration&
                                       aElapsedTime) const
{
  // Initializes to null. We always return this object to benefit from
  // return-value-optimization.
  TimeStamp result;

  // Currently we may dispatch animationstart events before resolving
  // mStartTime if we have a delay <= 0. This will change in bug 1134163
  // but until then we should just use the latest refresh driver time as
  // the event timestamp in that case.
  if (!mEffect || mStartTime.IsNull()) {
    nsPresContext* presContext = GetPresContext();
    if (presContext) {
      result = presContext->RefreshDriver()->MostRecentRefresh();
    }
    return result;
  }

  result = AnimationTimeToTimeStamp(aElapsedTime +
                                    mEffect->SpecifiedTiming().mDelay);
  return result;
}

////////////////////////// nsAnimationManager ////////////////////////////

NS_IMPL_CYCLE_COLLECTION(nsAnimationManager, mEventDispatcher)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(nsAnimationManager, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(nsAnimationManager, Release)

nsIStyleRule*
nsAnimationManager::CheckAnimationRule(nsStyleContext* aStyleContext,
                                       mozilla::dom::Element* aElement)
{
  // Ignore animations for print or print preview, and for elements
  // that are not attached to the document tree.
  if (!mPresContext->IsDynamic() || !aElement->IsInComposedDoc()) {
    return nullptr;
  }

  // Everything that causes our animation data to change triggers a
  // style change, which in turn triggers a non-animation restyle.
  // Likewise, when we initially construct frames, we're not in a
  // style change, but also not in an animation restyle.

  const nsStyleDisplay* disp = aStyleContext->StyleDisplay();
  AnimationCollection* collection =
    GetAnimationCollection(aElement,
                           aStyleContext->GetPseudoType(),
                           false /* aCreateIfNeeded */);
  if (!collection &&
      disp->mAnimationNameCount == 1 &&
      disp->mAnimations[0].GetName().IsEmpty()) {
    return nullptr;
  }

  nsAutoAnimationMutationBatch mb(aElement->OwnerDoc());

  // build the animations list
  dom::DocumentTimeline* timeline = aElement->OwnerDoc()->Timeline();
  AnimationPtrArray newAnimations;
  if (!aStyleContext->IsInDisplayNoneSubtree()) {
    BuildAnimations(aStyleContext, aElement, timeline, newAnimations);
  }

  if (newAnimations.IsEmpty()) {
    if (collection) {
      collection->Destroy();
    }
    return nullptr;
  }

  if (collection) {
    EffectSet* effectSet =
      EffectSet::GetEffectSet(aElement, aStyleContext->GetPseudoType());
    if (effectSet) {
      effectSet->UpdateAnimationGeneration(mPresContext);
    }

    // Copy over the start times and (if still paused) pause starts
    // for each animation (matching on name only) that was also in the
    // old list of animations.
    // This means that we honor dynamic changes, which isn't what the
    // spec says to do, but WebKit seems to honor at least some of
    // them.  See
    // http://lists.w3.org/Archives/Public/www-style/2011Apr/0079.html
    // In order to honor what the spec said, we'd copy more data over
    // (or potentially optimize BuildAnimations to avoid rebuilding it
    // in the first place).
    if (!collection->mAnimations.IsEmpty()) {

      for (size_t newIdx = newAnimations.Length(); newIdx-- != 0;) {
        Animation* newAnim = newAnimations[newIdx];

        // Find the matching animation with this name in the old list
        // of animations.  We iterate through both lists in a backwards
        // direction which means that if there are more animations in
        // the new list of animations with a given name than in the old
        // list, it will be the animations towards the of the beginning of
        // the list that do not match and are treated as new animations.
        RefPtr<CSSAnimation> oldAnim;
        size_t oldIdx = collection->mAnimations.Length();
        while (oldIdx-- != 0) {
          CSSAnimation* a = collection->mAnimations[oldIdx]->AsCSSAnimation();
          MOZ_ASSERT(a, "All animations in the CSS Animation collection should"
                        " be CSSAnimation objects");
          if (a->AnimationName() ==
              newAnim->AsCSSAnimation()->AnimationName()) {
            oldAnim = a;
            break;
          }
        }
        if (!oldAnim) {
          // FIXME: Bug 1134163 - We shouldn't queue animationstart events
          // until the animation is actually ready to run. However, we
          // currently have some tests that assume that these events are
          // dispatched within the same tick as the animation is added
          // so we need to queue up any animationstart events from newly-created
          // animations.
          newAnim->AsCSSAnimation()->QueueEvents();
          continue;
        }

        bool animationChanged = false;

        // Update the old from the new so we can keep the original object
        // identity (and any expando properties attached to it).
        if (oldAnim->GetEffect() && newAnim->GetEffect()) {
          KeyframeEffectReadOnly* oldEffect = oldAnim->GetEffect();
          KeyframeEffectReadOnly* newEffect = newAnim->GetEffect();
          animationChanged =
            oldEffect->SpecifiedTiming() != newEffect->SpecifiedTiming() ||
            oldEffect->Properties() != newEffect->Properties();
          oldEffect->SetSpecifiedTiming(newEffect->SpecifiedTiming());
          oldEffect->CopyPropertiesFrom(*newEffect);
        }

        // Handle changes in play state. If the animation is idle, however,
        // changes to animation-play-state should *not* restart it.
        if (oldAnim->PlayState() != AnimationPlayState::Idle) {
          // CSSAnimation takes care of override behavior so that,
          // for example, if the author has called pause(), that will
          // override the animation-play-state.
          // (We should check newAnim->IsStylePaused() but that requires
          //  downcasting to CSSAnimation and we happen to know that
          //  newAnim will only ever be paused by calling PauseFromStyle
          //  making IsPausedOrPausing synonymous in this case.)
          if (!oldAnim->IsStylePaused() && newAnim->IsPausedOrPausing()) {
            oldAnim->PauseFromStyle();
            animationChanged = true;
          } else if (oldAnim->IsStylePaused() &&
                    !newAnim->IsPausedOrPausing()) {
            oldAnim->PlayFromStyle();
            animationChanged = true;
          }
        }

        oldAnim->CopyAnimationIndex(*newAnim->AsCSSAnimation());

        // Updating the effect timing above might already have caused the
        // animation to become irrelevant so only add a changed record if
        // the animation is still relevant.
        if (animationChanged && oldAnim->IsRelevant()) {
          nsNodeUtils::AnimationChanged(oldAnim);
        }

        // Replace new animation with the (updated) old one and remove the
        // old one from the array so we don't try to match it any more.
        //
        // Although we're doing this while iterating this is safe because
        // we're not changing the length of newAnimations and we've finished
        // iterating over the list of old iterations.
        newAnim->CancelFromStyle();
        newAnim = nullptr;
        newAnimations.ReplaceElementAt(newIdx, oldAnim);
        collection->mAnimations.RemoveElementAt(oldIdx);
      }
    }
  } else {
    collection = GetAnimationCollection(aElement,
                                        aStyleContext->GetPseudoType(),
                                        true /* aCreateIfNeeded */);
    for (Animation* animation : newAnimations) {
      // FIXME: Bug 1134163 - As above, we have shouldn't actually need to
      // queue events here. (But we do for now since some tests expect
      // animationstart events to be dispatched immediately.)
      animation->AsCSSAnimation()->QueueEvents();
    }
  }
  collection->mAnimations.SwapElements(newAnimations);

  // Cancel removed animations
  for (size_t newAnimIdx = newAnimations.Length(); newAnimIdx-- != 0; ) {
    newAnimations[newAnimIdx]->CancelFromStyle();
  }

  EffectCompositor::UpdateCascadeResults(aElement,
                                         aStyleContext->GetPseudoType(),
                                         aStyleContext);

  EffectCompositor::CascadeLevel cascadeLevel =
    EffectCompositor::CascadeLevel::Animations;
  mPresContext->EffectCompositor()
              ->MaybeUpdateAnimationRule(aElement,
                                         aStyleContext->GetPseudoType(),
                                         cascadeLevel);

  // We don't actually dispatch the pending events now.  We'll either
  // dispatch them the next time we get a refresh driver notification
  // or the next time somebody calls
  // nsPresShell::FlushPendingNotifications.
  if (mEventDispatcher.HasQueuedEvents()) {
    mPresContext->Document()->SetNeedStyleFlush();
  }

  return mPresContext->EffectCompositor()
                     ->GetAnimationRule(aElement,
                                        aStyleContext->GetPseudoType(),
                                        cascadeLevel);
}

void
nsAnimationManager::StopAnimationsForElement(
  mozilla::dom::Element* aElement,
  nsCSSPseudoElements::Type aPseudoType)
{
  MOZ_ASSERT(aElement);
  AnimationCollection* collection =
    GetAnimationCollection(aElement, aPseudoType, false /* aCreateIfNeeded */);
  if (!collection) {
    return;
  }

  nsAutoAnimationMutationBatch mb(aElement->OwnerDoc());
  collection->Destroy();
}

struct KeyframeData {
  float mKey;
  uint32_t mIndex; // store original order since sort algorithm is not stable
  nsCSSKeyframeRule *mRule;
};

struct KeyframeDataComparator {
  bool Equals(const KeyframeData& A, const KeyframeData& B) const {
    return A.mKey == B.mKey && A.mIndex == B.mIndex;
  }
  bool LessThan(const KeyframeData& A, const KeyframeData& B) const {
    return A.mKey < B.mKey || (A.mKey == B.mKey && A.mIndex < B.mIndex);
  }
};

class ResolvedStyleCache {
public:
  ResolvedStyleCache() : mCache() {}
  nsStyleContext* Get(nsPresContext *aPresContext,
                      nsStyleContext *aParentStyleContext,
                      Declaration* aKeyframeDeclaration);

private:
  nsRefPtrHashtable<nsPtrHashKey<Declaration>, nsStyleContext> mCache;
};

nsStyleContext*
ResolvedStyleCache::Get(nsPresContext *aPresContext,
                        nsStyleContext *aParentStyleContext,
                        Declaration* aKeyframeDeclaration)
{
  // FIXME (spec):  The css3-animations spec isn't very clear about how
  // properties are resolved when they have values that depend on other
  // properties (e.g., values in 'em').  I presume that they're resolved
  // relative to the other styles of the element.  The question is
  // whether they are resolved relative to other animations:  I assume
  // that they're not, since that would prevent us from caching a lot of
  // data that we'd really like to cache (in particular, the
  // StyleAnimationValue values in AnimationPropertySegment).
  nsStyleContext *result = mCache.GetWeak(aKeyframeDeclaration);
  if (!result) {
    aKeyframeDeclaration->SetImmutable();
    // The spec says that !important declarations should just be ignored
    MOZ_ASSERT(!aKeyframeDeclaration->HasImportantData(),
               "Keyframe rule has !important data");

    nsCOMArray<nsIStyleRule> rules;
    rules.AppendObject(aKeyframeDeclaration);
    RefPtr<nsStyleContext> resultStrong = aPresContext->StyleSet()->
      ResolveStyleByAddingRules(aParentStyleContext, rules);
    mCache.Put(aKeyframeDeclaration, resultStrong);
    result = resultStrong;
  }
  return result;
}

void
nsAnimationManager::BuildAnimations(nsStyleContext* aStyleContext,
                                    dom::Element* aTarget,
                                    dom::AnimationTimeline* aTimeline,
                                    AnimationPtrArray& aAnimations)
{
  MOZ_ASSERT(aAnimations.IsEmpty(), "expect empty array");

  ResolvedStyleCache resolvedStyles;

  const nsStyleDisplay *disp = aStyleContext->StyleDisplay();

  RefPtr<nsStyleContext> styleWithoutAnimation;

  for (size_t animIdx = 0, animEnd = disp->mAnimationNameCount;
       animIdx != animEnd; ++animIdx) {
    const StyleAnimation& src = disp->mAnimations[animIdx];

    // CSS Animations whose animation-name does not match a @keyframes rule do
    // not generate animation events. This includes when the animation-name is
    // "none" which is represented by an empty name in the StyleAnimation.
    // Since such animations neither affect style nor dispatch events, we do
    // not generate a corresponding Animation for them.
    nsCSSKeyframesRule* rule =
      src.GetName().IsEmpty()
      ? nullptr
      : mPresContext->StyleSet()->KeyframesRuleForName(src.GetName());
    if (!rule) {
      continue;
    }

    RefPtr<CSSAnimation> dest =
      new CSSAnimation(mPresContext->Document()->GetScopeObject(),
                       src.GetName());
    dest->SetOwningElement(
      OwningElementRef(*aTarget, aStyleContext->GetPseudoType()));
    dest->SetTimeline(aTimeline);
    dest->SetAnimationIndex(static_cast<uint64_t>(animIdx));
    aAnimations.AppendElement(dest);

    TimingParams timing;
    timing.mDuration.SetAsUnrestrictedDouble() = src.GetDuration();
    timing.mDelay = TimeDuration::FromMilliseconds(src.GetDelay());
    timing.mIterations = src.GetIterationCount();
    timing.mDirection = src.GetDirection();
    timing.mFill = src.GetFillMode();

    RefPtr<KeyframeEffectReadOnly> destEffect =
      new KeyframeEffectReadOnly(mPresContext->Document(), aTarget,
                                 aStyleContext->GetPseudoType(), timing);
    dest->SetEffect(destEffect);

    if (src.GetPlayState() == NS_STYLE_ANIMATION_PLAY_STATE_PAUSED) {
      dest->PauseFromStyle();
    } else {
      dest->PlayFromStyle();
    }

    // While current drafts of css3-animations say that later keyframes
    // with the same key entirely replace earlier ones (no cascading),
    // this is a bad idea and contradictory to the rest of CSS.  So
    // we're going to keep all the keyframes for each key and then do
    // the replacement on a per-property basis rather than a per-rule
    // basis, just like everything else in CSS.

    AutoInfallibleTArray<KeyframeData, 16> sortedKeyframes;

    for (uint32_t ruleIdx = 0, ruleEnd = rule->StyleRuleCount();
         ruleIdx != ruleEnd; ++ruleIdx) {
      css::Rule* cssRule = rule->GetStyleRuleAt(ruleIdx);
      MOZ_ASSERT(cssRule, "must have rule");
      MOZ_ASSERT(cssRule->GetType() == css::Rule::KEYFRAME_RULE,
                 "must be keyframe rule");
      nsCSSKeyframeRule *kfRule = static_cast<nsCSSKeyframeRule*>(cssRule);

      const nsTArray<float> &keys = kfRule->GetKeys();
      for (uint32_t keyIdx = 0, keyEnd = keys.Length();
           keyIdx != keyEnd; ++keyIdx) {
        float key = keys[keyIdx];
        // FIXME (spec):  The spec doesn't say what to do with
        // out-of-range keyframes.  We'll ignore them.
        if (0.0f <= key && key <= 1.0f) {
          KeyframeData *data = sortedKeyframes.AppendElement();
          data->mKey = key;
          data->mIndex = ruleIdx;
          data->mRule = kfRule;
        }
      }
    }

    sortedKeyframes.Sort(KeyframeDataComparator());

    if (sortedKeyframes.Length() == 0) {
      // no segments
      continue;
    }

    // Record the properties that are present in any keyframe rules we
    // are using.
    nsCSSPropertySet properties;

    for (uint32_t kfIdx = 0, kfEnd = sortedKeyframes.Length();
         kfIdx != kfEnd; ++kfIdx) {
      css::Declaration *decl = sortedKeyframes[kfIdx].mRule->Declaration();
      for (uint32_t propIdx = 0, propEnd = decl->Count();
           propIdx != propEnd; ++propIdx) {
        nsCSSProperty prop = decl->GetPropertyAt(propIdx);
        if (prop != eCSSPropertyExtra_variable) {
          // CSS Variables are not animatable
          properties.AddProperty(prop);
        }
      }
    }

    for (nsCSSProperty prop = nsCSSProperty(0);
         prop < eCSSProperty_COUNT_no_shorthands;
         prop = nsCSSProperty(prop + 1)) {
      if (!properties.HasProperty(prop) ||
          nsCSSProps::kAnimTypeTable[prop] == eStyleAnimType_None) {
        continue;
      }

      // Build a list of the keyframes to use for this property.  This
      // means we need every keyframe with the property in it, except
      // for those keyframes where a later keyframe with the *same key*
      // also has the property.
      AutoInfallibleTArray<uint32_t, 16> keyframesWithProperty;
      float lastKey = 100.0f; // an invalid key
      for (uint32_t kfIdx = 0, kfEnd = sortedKeyframes.Length();
           kfIdx != kfEnd; ++kfIdx) {
        KeyframeData &kf = sortedKeyframes[kfIdx];
        if (!kf.mRule->Declaration()->HasProperty(prop)) {
          continue;
        }
        if (kf.mKey == lastKey) {
          // Replace previous occurrence of same key.
          keyframesWithProperty[keyframesWithProperty.Length() - 1] = kfIdx;
        } else {
          keyframesWithProperty.AppendElement(kfIdx);
        }
        lastKey = kf.mKey;
      }

      AnimationProperty &propData = *destEffect->Properties().AppendElement();
      propData.mProperty = prop;

      KeyframeData *fromKeyframe = nullptr;
      RefPtr<nsStyleContext> fromContext;
      bool interpolated = true;
      for (uint32_t wpIdx = 0, wpEnd = keyframesWithProperty.Length();
           wpIdx != wpEnd; ++wpIdx) {
        uint32_t kfIdx = keyframesWithProperty[wpIdx];
        KeyframeData &toKeyframe = sortedKeyframes[kfIdx];

        RefPtr<nsStyleContext> toContext =
          resolvedStyles.Get(mPresContext, aStyleContext,
                             toKeyframe.mRule->Declaration());

        if (fromKeyframe) {
          interpolated = interpolated &&
            BuildSegment(propData.mSegments, prop, src,
                         fromKeyframe->mKey, fromContext,
                         fromKeyframe->mRule->Declaration(),
                         toKeyframe.mKey, toContext);
        } else {
          if (toKeyframe.mKey != 0.0f) {
            // There's no data for this property at 0%, so use the
            // cascaded value above us.
            if (!styleWithoutAnimation) {
              styleWithoutAnimation = mPresContext->StyleSet()->
                ResolveStyleWithoutAnimation(aTarget, aStyleContext,
                                             eRestyle_AllHintsWithAnimations);
            }
            interpolated = interpolated &&
              BuildSegment(propData.mSegments, prop, src,
                           0.0f, styleWithoutAnimation, nullptr,
                           toKeyframe.mKey, toContext);
          }
        }

        fromContext = toContext;
        fromKeyframe = &toKeyframe;
      }

      if (fromKeyframe->mKey != 1.0f) {
        // There's no data for this property at 100%, so use the
        // cascaded value above us.
        if (!styleWithoutAnimation) {
          styleWithoutAnimation = mPresContext->StyleSet()->
            ResolveStyleWithoutAnimation(aTarget, aStyleContext,
                                         eRestyle_AllHintsWithAnimations);
        }
        interpolated = interpolated &&
          BuildSegment(propData.mSegments, prop, src,
                       fromKeyframe->mKey, fromContext,
                       fromKeyframe->mRule->Declaration(),
                       1.0f, styleWithoutAnimation);
      }

      // If we failed to build any segments due to inability to
      // interpolate, remove the property from the animation.  (It's not
      // clear if this is the right thing to do -- we could run some of
      // the segments, but it's really not clear whether we should skip
      // values (which?) or skip segments, so best to skip the whole
      // thing for now.)
      if (!interpolated) {
        destEffect->Properties().RemoveElementAt(
          destEffect->Properties().Length() - 1);
      }
    }
  }
}

bool
nsAnimationManager::BuildSegment(InfallibleTArray<AnimationPropertySegment>&
                                   aSegments,
                                 nsCSSProperty aProperty,
                                 const StyleAnimation& aAnimation,
                                 float aFromKey, nsStyleContext* aFromContext,
                                 mozilla::css::Declaration* aFromDeclaration,
                                 float aToKey, nsStyleContext* aToContext)
{
  StyleAnimationValue fromValue, toValue, dummyValue;
  if (!ExtractComputedValueForTransition(aProperty, aFromContext, fromValue) ||
      !ExtractComputedValueForTransition(aProperty, aToContext, toValue) ||
      // Check that we can interpolate between these values
      // (If this is ever a performance problem, we could add a
      // CanInterpolate method, but it seems fine for now.)
      !StyleAnimationValue::Interpolate(aProperty, fromValue, toValue,
                                        0.5, dummyValue)) {
    return false;
  }

  AnimationPropertySegment &segment = *aSegments.AppendElement();

  segment.mFromValue = fromValue;
  segment.mToValue = toValue;
  segment.mFromKey = aFromKey;
  segment.mToKey = aToKey;
  const nsTimingFunction *tf;
  if (aFromDeclaration &&
      aFromDeclaration->HasProperty(eCSSProperty_animation_timing_function)) {
    tf = &aFromContext->StyleDisplay()->mAnimations[0].GetTimingFunction();
  } else {
    tf = &aAnimation.GetTimingFunction();
  }
  segment.mTimingFunction.Init(*tf);

  return true;
}
