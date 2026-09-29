# usdGenPomadeTools.pomadeLadder -- the fallback ladder (plan/18 section 3.7,
# plan/17 section 7).
#
# Qt-free and session-shaped: the controller times each move end to end and
# hands the milliseconds here, and this module decides what fidelity the
# viewport gives up to get back inside the TN-1 budget. Nothing here reads a
# clock, so a test drives it with the timings it wants.
#
# The rungs, in the order plan/18 section 3.7 sets them:
#
#   1  preview fraction 0.25       the live fill thins out
#   2  preview fraction 0.10
#   3  preview fraction 0          no live fill at all
#   4  display segments halved     coarser tessellation
#   5  non-edited levels as centers only (Pomade_SetLevelDrawMode)
#   6  hover off                   no highlight work per sample
#
# Each rung costs three CONSECUTIVE moves over the budget, so one slow
# frame -- a stage swap landing, a window resize -- never drops fidelity;
# a drag that is genuinely too heavy walks down in about a fifth of a
# second. Release restores every value the ladder touched, from the base it
# captured at press, and republishes everything.
#
# One divergence from the plan text, recorded rather than hidden: rung 4
# says "halved outside the edited subtree", but Pomade_SetDisplaySegments is
# one number for the whole model (pomadeApi.h), so the halving is global.
# The edited subtree keeps its fidelity in the rung that can say so: rung 5
# leaves the focused level alone and takes every other level down to its
# center curves.
#
# Rung 6 ("hover off") cannot act mid-drag -- the controller never hovers
# under a live gesture -- so it is what the release does that gives it a
# meaning (FB-02): a drag that reached it keeps hover off for
# HOVER_COOLDOWN_MS after the release, which is the window the full-fidelity
# republish needs to land before per-sample highlight work resumes on top of
# it.  The caller hands the clock in (restore(nowMs), hoverCoolingDown(nowMs))
# so this module still reads none.
#
# FB-02 also puts the ladder under the artist's control: the tool state's
# `ladderEnabled` (the dock's "Ladder enabled" box) and `moveBudgetMs` are
# read at each press, and `chipLabel()` is the words the viewport HUD shows
# while a rung is live.
from __future__ import annotations

import ctypes

from . import pomadeLib

# plan/18 section 3.7 / plan/17 TN-1: a move that takes longer than this is
# over budget.
MOVE_BUDGET_MS = 8.0
# ... and this many consecutive over-budget moves step the ladder.
TRIGGER_MOVES = 3

STEP_FULL = 0
STEP_PREVIEW_25 = 1
STEP_PREVIEW_10 = 2
STEP_PREVIEW_OFF = 3
STEP_HALF_SEGMENTS = 4
STEP_CENTERS_ONLY = 5
STEP_HOVER_OFF = 6
MAX_STEP = STEP_HOVER_OFF

STEP_LABELS = (
    "full",
    "preview 25%",
    "preview 10%",
    "preview off",
    "half segments",
    "centers only",
    "hover off",
)

# The HUD chip's words for each rung (FB-02): capitalised, with the spaced
# percent the rest of the dock uses, and "(auto)" so the artist knows the
# tool did it and it will come back by itself.
CHIP_LABELS = (
    "",
    "Preview 25 %",
    "Preview 10 %",
    "Preview off",
    "Half segments",
    "Centers only",
    "Hover off",
)

# After a release from the hover-off rung, hover stays off this long.
HOVER_COOLDOWN_MS = 500.0


def chipLabel(step):
    """The HUD chip for `step`, e.g. 'Preview 10 % (auto)'; '' at full."""
    step = int(step)
    if step <= STEP_FULL:
        return ""
    return "%s (auto)" % CHIP_LABELS[min(step, MAX_STEP)]

# The preview fraction each of the first three rungs asks for.
_PREVIEW_AT = {STEP_PREVIEW_25: 0.25, STEP_PREVIEW_10: 0.10,
               STEP_PREVIEW_OFF: 0.0}

# How many tube ids a level scan reads at once (a groom with more tubes
# than this still scans: the probe reports the true count first).
_MAX_TUBES = 4096


def levelsPresent(session, maxLevel=0):
    """The hierarchy levels the model actually holds, ascending.

    Read once per press, from the tube ids and their levels; an empty
    answer (a headless fake, a model with no tube) falls back to
    1..maxLevel so the caller still has something to walk.
    """
    dll = getattr(session, "dll", None)
    model = getattr(session, "model", None)
    levels = set()
    if dll is not None and model is not None:
        count = ctypes.c_int(0)
        try:
            dll.Pomade_ReadTubeIds(model, None, 0, ctypes.byref(count))
            n = min(max(int(count.value), 0), _MAX_TUBES)
            if n > 0:
                ids = (ctypes.c_int * n)()
                got = ctypes.c_int(0)
                if dll.Pomade_ReadTubeIds(model, ids, n,
                                         ctypes.byref(got)) == \
                        pomadeLib.POMADE_OK:
                    for i in range(min(n, int(got.value))):
                        level = int(dll.Pomade_GetTubeLevel(model, ids[i]))
                        if level >= 1:
                            levels.add(level)
        except (AttributeError, OSError, ValueError):
            levels = set()
    if not levels:
        return tuple(range(1, max(int(maxLevel), 0) + 1))
    return tuple(sorted(levels))


class FallbackLadder:
    """One ladder per controller; armed at press, restored at release."""

    def __init__(self, session, state, budgetMs=None,
                 trigger=TRIGGER_MOVES):
        self._session = session
        self._state = state
        # An explicit budget pins the ladder (a test's); None follows the
        # tool state's moveBudgetMs, re-read at every arm().
        self._fixedBudgetMs = (float(budgetMs) if budgetMs is not None
                               else None)
        self._budgetMs = self._stateBudget()
        self._trigger = max(int(trigger), 1)
        self._hoverCooldownUntil = 0.0   # caller's clock, milliseconds
        self._step = 0
        self._overBudget = 0
        self._armed = False
        self._editedLevel = 0
        self._basePreview = None
        self._baseSegments = None
        self._baseLevels = {}       # level -> (visible, xray, centersOnly)
        self._levels = None         # the levels arm() was told about
        self._firstTriggerMs = 0.0

    # -- state -------------------------------------------------------------

    @property
    def step(self):
        return self._step

    @property
    def armed(self):
        return self._armed

    @property
    def hoverSuppressed(self):
        """True once the ladder has turned hover work off (rung 6)."""
        return self._step >= STEP_HOVER_OFF

    @property
    def firstTriggerMs(self):
        """The move time that first stepped the ladder (0 = never has).

        Recorded because plan/17 section 7 wants the ladder's trigger
        measured rather than asserted.
        """
        return self._firstTriggerMs

    @property
    def budgetMs(self):
        """The move budget the live (or next) drag steps on."""
        return self._budgetMs

    def hoverCoolingDown(self, nowMs):
        """True inside the post-release window of a hover-off drag."""
        return float(nowMs) < self._hoverCooldownUntil

    def hoverCooldownRemainingMs(self, nowMs):
        return max(0.0, self._hoverCooldownUntil - float(nowMs))

    def label(self):
        return STEP_LABELS[min(self._step, MAX_STEP)]

    def chipLabel(self):
        """The HUD chip for the live rung; '' at full fidelity."""
        return chipLabel(self._step)

    def _stateBudget(self):
        if self._fixedBudgetMs is not None:
            return self._fixedBudgetMs
        try:
            return max(float(getattr(self._state, "moveBudgetMs",
                                     MOVE_BUDGET_MS)), 0.0)
        except (TypeError, ValueError):
            return MOVE_BUDGET_MS

    def describe(self):
        if self._step <= 0:
            return "fidelity: full"
        return ("fidelity: step %d/%d (%s)"
                % (self._step, MAX_STEP, self.label()))

    # -- the gesture bracket -----------------------------------------------

    def arm(self, editedLevel=0, levels=None):
        """Capture the base fidelity at press. Idempotent within a drag.

        Two numbers, two ABI reads: the level scan costs one call per tube
        and belongs to the rung that needs it, not to every press.

        With the tool state's `ladderEnabled` off nothing is armed, so the
        drag keeps full fidelity however slow it runs (FB-02).
        """
        if self._armed:
            return False
        if not bool(getattr(self._state, "ladderEnabled", True)):
            return False
        self._budgetMs = self._stateBudget()
        self._armed = True
        self._step = 0
        self._overBudget = 0
        self._editedLevel = int(editedLevel)
        self._basePreview = self._readPreview()
        self._baseSegments = self._readSegments()
        self._baseLevels = {}
        self._levels = tuple(levels) if levels is not None else None
        self._syncState()
        return True

    def noteMove(self, elapsedMs):
        """One timed move. True when the ladder stepped on this one."""
        if not self._armed or self._step >= MAX_STEP:
            if self._armed and float(elapsedMs) <= self._budgetMs:
                self._overBudget = 0
            return False
        if float(elapsedMs) <= self._budgetMs:
            self._overBudget = 0
            return False
        self._overBudget += 1
        if self._overBudget < self._trigger:
            return False
        self._overBudget = 0
        self._step += 1
        if self._firstTriggerMs <= 0.0:
            self._firstTriggerMs = float(elapsedMs)
        self._applyStep(self._step)
        self._syncState()
        return True

    def restore(self, nowMs=None):
        """Release: put every value back and republish at full fidelity.

        `nowMs` (the caller's clock) starts the hover cool-down when the
        drag had reached the hover-off rung.
        """
        if not self._armed:
            return False
        stepped = self._step > 0
        if self._step >= STEP_HOVER_OFF and nowMs is not None:
            self._hoverCooldownUntil = float(nowMs) + HOVER_COOLDOWN_MS
        if stepped:
            if self._basePreview is not None:
                self._writePreview(self._basePreview)
            if self._baseSegments is not None:
                self._writeSegments(self._baseSegments)
            for level, base in self._baseLevels.items():
                if base is not None:
                    self._writeLevel(level, base[0], base[1], base[2])
        self._armed = False
        self._step = 0
        self._overBudget = 0
        self._editedLevel = 0
        self._syncState()
        if stepped:
            self._publish(pomadeLib.POMADE_DIRTY_ALL)
        return stepped

    # -- rungs --------------------------------------------------------------

    def _applyStep(self, step):
        if step in _PREVIEW_AT:
            self._writePreview(_PREVIEW_AT[step])
            return
        if step == STEP_HALF_SEGMENTS:
            base = self._baseSegments if self._baseSegments else 1
            self._writeSegments(max(1, int(base) // 2))
            return
        if step == STEP_CENTERS_ONLY:
            found = (self._levels if self._levels is not None
                     else levelsPresent(self._session, self._editedLevel))
            for level in found:
                self._baseLevels.setdefault(int(level),
                                            self._readLevel(int(level)))
            for level, base in self._baseLevels.items():
                if level == self._editedLevel or base is None:
                    continue
                self._writeLevel(level, base[0], base[1], True)
            self._publish(pomadeLib.POMADE_DIRTY_DISPLAY)
            return
        if step == STEP_HOVER_OFF:
            session = self._session
            if session is not None and hasattr(session, "setHover"):
                session.setHover(0, -1, -1, -1)
                self._publish(pomadeLib.POMADE_DIRTY_SELECTION)

    def _syncState(self):
        if self._state is not None:
            self._state.ladderStep = int(self._step)

    # -- the model -----------------------------------------------------------

    def _dll(self):
        dll = getattr(self._session, "dll", None) if self._session else None
        model = getattr(self._session, "model", None) if self._session \
            else None
        if dll is None or model is None:
            return None, None
        return dll, model

    def _publish(self, mask):
        session = self._session
        if session is not None and hasattr(session, "publish"):
            session.publish(mask)

    def _readPreview(self):
        dll, model = self._dll()
        if dll is None:
            return None
        try:
            return float(dll.Pomade_GetPreviewFraction(model))
        except (AttributeError, OSError, ValueError):
            return None

    def _writePreview(self, fraction):
        dll, model = self._dll()
        if dll is None:
            return
        dll.Pomade_SetPreviewFraction(model, ctypes.c_float(float(fraction)))
        if self._state is not None:
            self._state.previewFraction = float(fraction)

    def _readSegments(self):
        dll, model = self._dll()
        if dll is None:
            return None
        try:
            return int(dll.Pomade_GetDisplaySegments(model))
        except (AttributeError, OSError, ValueError):
            return None

    def _writeSegments(self, segments):
        dll, model = self._dll()
        if dll is None:
            return
        dll.Pomade_SetDisplaySegments(model, int(segments))
        if self._state is not None:
            self._state.displaySegments = int(segments)

    def _readLevel(self, level):
        dll, model = self._dll()
        if dll is None:
            return None
        visible = ctypes.c_int(1)
        xray = ctypes.c_int(0)
        centers = ctypes.c_int(0)
        try:
            if dll.Pomade_GetLevelDrawMode(
                    model, int(level), ctypes.byref(visible),
                    ctypes.byref(xray),
                    ctypes.byref(centers)) != pomadeLib.POMADE_OK:
                return None
        except (AttributeError, OSError, ValueError):
            return None
        return (bool(visible.value), bool(xray.value), bool(centers.value))

    def _writeLevel(self, level, visible, xray, centersOnly):
        dll, model = self._dll()
        if dll is None:
            return
        dll.Pomade_SetLevelDrawMode(model, int(level), 1 if visible else 0,
                                   1 if xray else 0, 1 if centersOnly else 0)
