# usdGenTonicTools.tonicWorkspace -- the Tonic dock (plan/18 section 3.5).
#
# One QDockWidget: a mode shelf, the active mode's sub-mode shelf, a
# generated parameter form (tonicPanels.descriptors), an actions block
# (tonicPanels.actions plus Save/Export/Import, which need a QFileDialog),
# a warnings list (tonicHud.warnings) and a status strip
# (tonicHud.statusStrip). Nothing here computes a value or drives the ABI
# directly except the three file-dialog actions: every parameter edit and
# one-shot action goes through the Qt-free descriptors in tonicPanels.py, so
# the dock itself has no logic to unit test.
#
# Follows the ExpressionEditorDock pattern (usdGenTools/exprEditor.py): a
# QDockWidget added to usdviewApi.qMainWindow's right dock area, Qt imported
# only from this module (pxr.Usdviewq.qt), never at import time from any
# sibling that other code loads headlessly.
from __future__ import annotations

import time

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

from . import tonicHierarchy, tonicHud, tonicModes, tonicPanels

TITLE = "Tonic"
REFRESH_MS = 250

# Sub-mode shelf source per mode: the *_SUBMODES tuple, the setter that
# records a click on `state`, and whether the shelf shows at all (plan/18
# section 3.5 item 2: "hidden for Output").
_SUBMODES_BY_MODE = {
    "graph": (tonicModes.GRAPH_SUBMODES, tonicModes.SetActiveGraphSubMode),
    "tube": (tonicModes.TUBE_SUBMODES, tonicModes.SetActiveTubeSubMode),
    "fill": (tonicModes.FILL_SUBMODES, tonicModes.SetActiveFillSubMode),
    "hierarchy": (tonicModes.HIERARCHY_SUBMODES,
                 tonicModes.SetActiveHierarchySubMode),
    "sculpt": (tonicModes.SCULPT_SUBMODES, tonicModes.SetActiveSculptSubMode),
}

_SUBMODE_STATE_ATTR = {
    "graph": "graphSubMode",
    "tube": "tubeSubMode",
    "fill": "fillSubMode",
    "hierarchy": "hierarchySubMode",
    "sculpt": "sculptSubMode",
}

_SEVERITY_PREFIX = {"error": "[!!]", "warning": "[!]", "info": "[i]"}

_WORKSPACES = {}


def openWorkspace(container, usdviewApi):
    """Show the process-wide Tonic dock for `container`, creating it once.

    Idempotent: a second call re-shows, raises and refreshes the existing
    dock instead of building a duplicate (plan/18 section 2 module-map
    contract).
    """
    key = id(container)
    workspace = _WORKSPACES.get(key)
    if workspace is not None:
        try:
            workspace.show()
            workspace.raise_()
            workspace.refresh()
            return workspace
        except RuntimeError:
            # The underlying C++ QDockWidget was already destroyed (a
            # stage reload tore down the main window's docks).
            del _WORKSPACES[key]
    workspace = TonicWorkspace(container, usdviewApi)
    _WORKSPACES[key] = workspace
    return workspace


def _escape(text):
    """Plain text inside the status strip's rich text."""
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def _ramp_to_text(pairs):
    pairs = list(pairs)
    return ", ".join(
        "%.4g:%.4g" % (pairs[i], pairs[i + 1])
        for i in range(0, len(pairs) - 1, 2))


def _paramSignature(descriptors):
    """What a generated parameter form is built from.

    A cached per-mode page is reusable exactly while this is unchanged.
    Only the hierarchy panel varies at all, and only in two row labels
    that name the active level.
    """
    return tuple(
        (d.id, d.label, d.kind, d.min, d.max, d.step,
         tuple(d.choices) if d.choices else ())
        for d in descriptors)


def _text_to_ramp(text):
    flat = []
    for token in text.split(","):
        token = token.strip()
        if not token:
            continue
        pos, _, val = token.partition(":")
        flat.append(float(pos))
        flat.append(float(val))
    return flat


class TonicWorkspace(QtWidgets.QDockWidget):

    def __init__(self, container, usdviewApi):
        super(TonicWorkspace, self).__init__(TITLE, usdviewApi.qMainWindow)
        self.setObjectName("usdGenTonicWorkspace")
        self._container = container
        self._api = usdviewApi
        self._activeMode = ""
        self._paramWidgets = []   # [(descriptor, widget)] of the live page
        # modeId -> {subMode, subButtons, subGroup, params, paramRows,
        # paramSignature, actions, hasSubModes}. Built on first use, kept
        # for the life of the dock, switched with setCurrentWidget.
        self._pages = {}
        self._pageLevel = 0
        # What the last refresh actually showed. refresh() is called on
        # every publish, on every idle pump and on a 250 ms timer -- about
        # four times per artist op in the TN-5 controller soak -- so each
        # part compares its own inputs first and does nothing when they
        # have not moved (plan/17 section 7, the V9 note).
        self._shelfKey = None
        self._warningsKey = None
        self._warningsTexts = None
        self._statusText = None
        self._crumbText = None
        self._statusAmber = None
        self._staleContent = False
        self._warningsAt = 0.0
        self._buildUi()
        self._api.qMainWindow.addDockWidget(
            QtCore.Qt.RightDockWidgetArea, self)
        self._timer = QtCore.QTimer(self)
        self._timer.setInterval(REFRESH_MS)
        self._timer.timeout.connect(self._onTimerTick)
        self.visibilityChanged.connect(self._onVisibilityChanged)
        state = container.tonicState
        self._setActiveMode(state.activeMode or tonicModes.MODES[0].id)
        state.workspaceOpen = True
        self.show()

    # ---- what the dock is showing ----------------------------------------
    #
    # Three read-only answers, for the T3 script that has to tell "the
    # panel rebuilt itself" from "the state changed and the panel did
    # not". Qt offers no way to read a QFormLayout back as descriptors.

    @property
    def activeMode(self):
        """The mode whose shelf, rows and actions are currently built."""
        return self._activeMode

    def parameterIds(self):
        """The descriptor ids of the parameter rows on screen."""
        return [descriptor.id for descriptor, _w in self._paramWidgets]

    def subModeIds(self):
        """The sub-mode ids on the shelf under the mode buttons."""
        return list(self._subModeButtons)

    # ---- why nothing here is retired any more -----------------------------
    #
    # This dock used to tear the sub-mode shelf, the parameter form and
    # the action buttons down on every mode switch and hold the old
    # widgets in a `_retired` list until the next rebuild freed them.
    # Two costs came with that, both measured on the TN-5 controller soak
    # (plan/17 section 7, the V9 note):
    #
    #  * the free never happened. `_dropRetired` called deleteLater(),
    #    and Qt will not deliver a DeferredDelete posted at event-loop
    #    level 0 from inside a nested processEvents() -- which is every
    #    turn a T3 script gives it. The event sat in the posted queue
    #    holding the receiver, so each retired QToolButton, its connected
    #    lambdas and PySide's per-connection weakrefs lived for the life
    #    of the process: +60 Python objects per artist op, 285 000 live
    #    objects and +380 MB of private bytes in three minutes, with the
    #    Qt object count under the main window flat the whole time
    #    because a retired widget has no parent to be counted under.
    #  * the rebuild itself was about sixteen widgets created and
    #    destroyed per mode switch, and the relayout cascade behind it
    #    was most of the dock's share of the event traffic.
    #
    # Each mode now has one page per stack, built on first use and kept.
    # A mode switch is setCurrentWidget: nothing is created, nothing is
    # destroyed, and there is nothing to retire.

    # ---- construction ----------------------------------------------------

    def _buildUi(self):
        body = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(body)
        layout.setSpacing(4)

        # 1. Mode shelf (plan/18 section 3.5 item 1): six checkable
        # QToolButtons, hotkey 1-6 in the tooltip and the label.
        modeRow = QtWidgets.QHBoxLayout()
        self._modeGroup = QtWidgets.QButtonGroup(self)
        self._modeGroup.setExclusive(True)
        self._modeButtons = {}
        for mode in tonicModes.MODES:
            btn = QtWidgets.QToolButton()
            btn.setText("%s %s" % (mode.hotkey, mode.label))
            btn.setCheckable(True)
            btn.setToolTip("%s (%s)\n%s"
                          % (mode.label, mode.hotkey, mode.status))
            btn.clicked.connect(
                lambda checked, m=mode.id: self._setActiveMode(m))
            self._modeGroup.addButton(btn)
            self._modeButtons[mode.id] = btn
            modeRow.addWidget(btn)
        layout.addLayout(modeRow)

        # 2-4. Sub-mode shelf, generated parameter form and action
        # buttons. Each mode gets ONE page in each stack, built the first
        # time that mode is shown and kept; switching modes is
        # setCurrentWidget, which creates and destroys nothing.
        #
        # They used to be torn down and rebuilt on every mode switch. That
        # is about sixteen widgets created and destroyed each time, and in
        # the TN-5 controller soak -- where three ops in four press a mode
        # key -- it drove 13 Create and 13 Destroy events per artist op
        # and a relayout cascade over the whole dock: 140 Move, 92 Resize,
        # 40 Paint events per op, every one of them through two Python
        # application event filters.
        self._subModeStack = QtWidgets.QStackedWidget()
        self._subModeStack.setContentsMargins(0, 0, 0, 0)
        self._subModeGroup = None
        self._subModeButtons = {}
        layout.addWidget(self._subModeStack)

        paramsBox = QtWidgets.QGroupBox("Parameters")
        paramsOuter = QtWidgets.QVBoxLayout(paramsBox)
        paramsOuter.setContentsMargins(0, 0, 0, 0)
        self._paramsStack = QtWidgets.QStackedWidget()
        paramsOuter.addWidget(self._paramsStack)
        layout.addWidget(paramsBox)

        # Output mode's actions include the three file-dialog buttons
        # Save/Export/Import, which need Qt and so are wired here.
        actionsBox = QtWidgets.QGroupBox("Actions")
        actionsOuter = QtWidgets.QVBoxLayout(actionsBox)
        actionsOuter.setContentsMargins(0, 0, 0, 0)
        self._actionsStack = QtWidgets.QStackedWidget()
        actionsOuter.addWidget(self._actionsStack)
        layout.addWidget(actionsBox)

        # 5. Warnings.
        warningsBox = QtWidgets.QGroupBox("Warnings")
        warningsLayout = QtWidgets.QVBoxLayout(warningsBox)
        self._warningsList = QtWidgets.QListWidget()
        self._warningsList.setMaximumHeight(96)
        self._warningsList.itemClicked.connect(self._onWarningClicked)
        warningsLayout.addWidget(self._warningsList)
        layout.addWidget(warningsBox)

        # 6. Status strip.
        statusRow = QtWidgets.QHBoxLayout()
        self._statusLabel = QtWidgets.QLabel()
        self._statusLabel.setWordWrap(True)
        # Both strip labels are pinned: an Ignored horizontal policy and a
        # fixed height mean neither size hint moves when the text does, so
        # setText cannot make the dock's nested layouts re-run. Unpinned,
        # the strip changes width on almost every publish (a version
        # number gains a digit, the swap time changes) and the relayout
        # sends a Move and a Resize to every widget in the dock -- 139
        # Move and 91 Resize events per artist op in the TN-5 controller
        # soak, each one through two Python application event filters.
        self._statusLabel.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                        QtWidgets.QSizePolicy.Fixed)
        self._statusLabel.setFixedHeight(
            self._statusLabel.fontMetrics().height() * 2)
        # The breadcrumb is a row of links (plan/18 section 3.5 item 6:
        # "breadcrumb (clickable)"): clicking L2 focuses level 2, which is
        # the same edit Ctrl+Down makes.
        self._statusLabel.setTextFormat(QtCore.Qt.RichText)
        self._statusLabel.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse |
            QtCore.Qt.LinksAccessibleByMouse)
        self._statusLabel.linkActivated.connect(self._onBreadcrumbClicked)
        statusRow.addWidget(self._statusLabel)
        # Versions, skew, swap time and the GPU/fallback note live in a
        # PLAIN-text label beside the breadcrumb. They change on almost
        # every publish, and setting rich text re-parses the HTML and
        # re-lays the label out each time -- measurable at four refreshes
        # per artist op. The breadcrumb next door is the only part that
        # needs the links, and it changes only when the level does.
        self._statusDetail = QtWidgets.QLabel()
        self._statusDetail.setWordWrap(True)
        self._statusDetail.setTextFormat(QtCore.Qt.PlainText)
        self._statusDetail.setSizePolicy(QtWidgets.QSizePolicy.Ignored,
                                         QtWidgets.QSizePolicy.Fixed)
        self._statusDetail.setFixedHeight(
            self._statusDetail.fontMetrics().height() * 2)
        statusRow.addWidget(self._statusDetail, 1)
        self._amplifiedCheck = QtWidgets.QCheckBox("Show amplified hair")
        self._amplifiedCheck.toggled.connect(self._onAmplifiedToggled)
        statusRow.addWidget(self._amplifiedCheck)
        layout.addLayout(statusRow)

        layout.addStretch(1)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(body)
        self.setWidget(scroll)

    # ---- mode / sub-mode switching ----------------------------------------

    def _setActiveMode(self, modeId):
        """Select `modeId`: records it on state, drives the viewport, drops
        the sub-mode to the mode's default (plan/18 section 3.5 item 1) and
        rebuilds the panel below the shelf."""
        state = self._container.tonicState
        tonicModes.SetActiveMode(state, modeId)
        viewport = getattr(self._container, "viewport", None)
        if viewport is not None and hasattr(viewport, "setMode"):
            viewport.setMode(modeId)
        submodes = _SUBMODES_BY_MODE.get(modeId)
        if submodes is not None:
            submodes[1](state, "")
            if viewport is not None and hasattr(viewport, "setSubMode"):
                viewport.setSubMode("")
        self._adoptMode(modeId)
        self.refresh()

    def _ensurePage(self, modeId, state):
        """The cached page set for `modeId`, built or rebuilt if needed."""
        page = self._pages.get(modeId)
        signature = _paramSignature(tonicPanels.descriptors(modeId, state))
        if page is not None and page["paramSignature"] == signature:
            return page
        if page is not None:
            # The rows themselves changed (the hierarchy panel names the
            # active level in two labels). Replace that one page; nothing
            # else in the dock is touched.
            old = page["params"]
            self._paramsStack.removeWidget(old)
            old.setParent(None)
            page["params"] = None
        else:
            subPage, subButtons, subGroup = self._buildSubModePage(modeId)
            self._subModeStack.addWidget(subPage)
            actionPage = self._buildActionPage(modeId)
            self._actionsStack.addWidget(actionPage)
            page = {"subMode": subPage, "subButtons": subButtons,
                    "subGroup": subGroup, "actions": actionPage,
                    "hasSubModes": modeId in _SUBMODES_BY_MODE}
            self._pages[modeId] = page
        paramPage, rows, signature = self._buildParamPage(modeId, state)
        self._paramsStack.addWidget(paramPage)
        page["params"] = paramPage
        page["paramRows"] = rows
        page["paramSignature"] = signature
        return page

    def _adoptMode(self, modeId):
        """Show the panel for `modeId` without driving anything.

        The half of _setActiveMode that is about widgets. refresh() calls
        it when the mode changed somewhere else, which is the usual case:
        the number keys go to the viewport controller, not to this dock
        (plan/18 section 3.4).
        """
        self._activeMode = modeId
        # The shelf below is about to change, so whatever refresh() last
        # synced is gone: force the next sync.
        self._shelfKey = None
        self._syncModeButtons()
        state = self._container.tonicState
        page = self._ensurePage(modeId, state)
        self._pageLevel = int(getattr(state, "activeLevel", 0))
        self._subModeStack.setCurrentWidget(page["subMode"])
        self._subModeStack.setVisible(page["hasSubModes"])
        self._paramsStack.setCurrentWidget(page["params"])
        self._actionsStack.setCurrentWidget(page["actions"])
        self._subModeButtons = page["subButtons"]
        self._subModeGroup = page["subGroup"]
        self._paramWidgets = page["paramRows"]

    def _syncModeButtons(self):
        for modeId, btn in self._modeButtons.items():
            btn.blockSignals(True)
            btn.setChecked(modeId == self._activeMode)
            btn.blockSignals(False)

    def _buildSubModePage(self, modeId):
        """The sub-mode shelf page for `modeId`, built once."""
        page = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(page)
        row.setContentsMargins(0, 0, 0, 0)
        buttons = {}
        group = None
        entry = _SUBMODES_BY_MODE.get(modeId)
        if entry is not None:
            submodes, setter = entry
            group = QtWidgets.QButtonGroup(page)
            group.setExclusive(True)
            for sub in submodes:
                btn = QtWidgets.QToolButton()
                btn.setText("%s %s" % (sub.hotkey, sub.label))
                btn.setCheckable(True)
                btn.setToolTip(sub.status)
                btn.clicked.connect(
                    lambda checked, s=sub.id, fn=setter:
                    self._onSubMode(fn, s))
                group.addButton(btn)
                buttons[sub.id] = btn
                row.addWidget(btn)
        return page, buttons, group

    def _onSubMode(self, setter, subId):
        state = self._container.tonicState
        setter(state, subId)
        viewport = getattr(self._container, "viewport", None)
        if viewport is not None and hasattr(viewport, "setSubMode"):
            viewport.setSubMode(subId)
        self._syncSubModeButtons()

    def _syncSubModeButtons(self):
        attr = _SUBMODE_STATE_ATTR.get(self._activeMode)
        current = getattr(self._container.tonicState, attr, "") if attr \
            else ""
        for subId, btn in self._subModeButtons.items():
            btn.blockSignals(True)
            btn.setChecked(subId == current)
            btn.blockSignals(False)

    # ---- parameters --------------------------------------------------

    def _buildParamPage(self, modeId, state):
        """The generated parameter form for `modeId`, built once.

        Returns the page, its [(descriptor, widget)] rows and the
        signature the rows were built from. Only `_hierarchyDescriptors`
        reads `state` at all, and only for the level in two row labels,
        so the signature is what decides whether a cached page is stale.
        """
        page = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(page)
        form.setContentsMargins(0, 0, 0, 0)
        rows = []
        descriptors = tonicPanels.descriptors(modeId, state)
        for descriptor in descriptors:
            widget = self._makeParamWidget(descriptor)
            form.addRow(descriptor.label, widget)
            rows.append((descriptor, widget))
        return page, rows, _paramSignature(descriptors)

    def _makeParamWidget(self, descriptor):
        if descriptor.kind == "int":
            w = QtWidgets.QSpinBox()
            w.setRange(int(descriptor.min), int(descriptor.max))
            w.setSingleStep(max(1, int(descriptor.step or 1)))
            w.valueChanged.connect(
                lambda v, d=descriptor: self._onParamChanged(d, v))
            return w
        if descriptor.kind == "float":
            w = QtWidgets.QDoubleSpinBox()
            w.setRange(float(descriptor.min), float(descriptor.max))
            w.setSingleStep(float(descriptor.step or 0.1))
            w.setDecimals(4)
            w.valueChanged.connect(
                lambda v, d=descriptor: self._onParamChanged(d, v))
            return w
        if descriptor.kind == "bool":
            w = QtWidgets.QCheckBox()
            w.toggled.connect(
                lambda v, d=descriptor: self._onParamChanged(d, v))
            return w
        if descriptor.kind == "enum":
            w = QtWidgets.QComboBox()
            for choice in descriptor.choices:
                w.addItem(str(choice), choice)
            w.currentIndexChanged.connect(
                lambda i, d=descriptor, box=w:
                self._onParamChanged(d, box.itemData(i)))
            return w
        if descriptor.kind == "ramp":
            w = QtWidgets.QLineEdit()
            w.setPlaceholderText("pos:val, pos:val ... (empty = uniform)")
            w.editingFinished.connect(
                lambda d=descriptor, edit=w: self._onParamChanged(
                    d, _text_to_ramp(edit.text())))
            return w
        raise ValueError("tonicWorkspace: unknown descriptor kind %r"
                         % (descriptor.kind,))

    def _onParamChanged(self, descriptor, value):
        session = getattr(self._container, "session", None)
        descriptor.set(self._container.tonicState, session, value)
        self.refresh()

    def _refreshParams(self, session):
        state = self._container.tonicState
        for descriptor, widget in self._paramWidgets:
            value = descriptor.get(state, session)
            widget.blockSignals(True)
            if descriptor.kind == "int":
                widget.setValue(int(value))
            elif descriptor.kind == "float":
                widget.setValue(float(value))
            elif descriptor.kind == "bool":
                widget.setChecked(bool(value))
            elif descriptor.kind == "enum":
                idx = widget.findData(value)
                widget.setCurrentIndex(max(idx, 0))
            elif descriptor.kind == "ramp":
                widget.setText(_ramp_to_text(value))
            widget.blockSignals(False)

    # ---- actions ----------------------------------------------------------

    def _buildActionPage(self, modeId):
        """The one-shot action buttons for `modeId`, built once."""
        page = QtWidgets.QWidget()
        column = QtWidgets.QVBoxLayout(page)
        column.setContentsMargins(0, 0, 0, 0)
        for action in tonicPanels.actions(modeId):
            label = action.label
            if action.hotkeyLabel:
                label = "%s (%s)" % (label, action.hotkeyLabel)
            btn = QtWidgets.QPushButton(label)
            btn.clicked.connect(
                lambda checked=False, a=action: self._onAction(a))
            column.addWidget(btn)
        if modeId == "output":
            for label, handler in (
                    ("Save groom...", self._onSaveGroom),
                    ("Export center curves...", self._onExportCenterCurves),
                    ("Import curves...", self._onImportCurves)):
                btn = QtWidgets.QPushButton(label)
                btn.clicked.connect(
                    lambda checked=False, fn=handler: fn())
                column.addWidget(btn)
        return page

    def _onAction(self, action):
        action.handler(self._container)
        self.refresh()

    def _onSaveGroom(self):
        session = getattr(self._container, "session", None)
        if session is None or not hasattr(session, "saveGroom"):
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save groom", "", "USD (*.usd *.usda *.usdc)")
        if path:
            session.saveGroom(path)
            self.refresh()

    def _onExportCenterCurves(self):
        session = getattr(self._container, "session", None)
        if session is None or not hasattr(session, "exportCenterCurves"):
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Export center curves", "", "USD (*.usd *.usda *.usdc)")
        if path:
            level = int(getattr(self._container.tonicState, "activeLevel",
                                1))
            session.exportCenterCurves(path, level)
            self.refresh()

    def _onImportCurves(self):
        session = getattr(self._container, "session", None)
        if session is None or not hasattr(session, "importCurves"):
            return
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Import curves", "", "USD (*.usd *.usda *.usdc)")
        if path:
            ids = tonicPanels.selectedTubeIds(session)
            parentTubeId = ids[0] if ids else -1
            session.importCurves(path, parentTubeId)
            self.refresh()

    def _onAmplifiedToggled(self, value):
        self._container.tonicState.showAmplifiedHair = bool(value)
        # plan/17 section 3.2: the model owns the swap, so the Tonic scene
        # index can hide the cook's amplified tiles and the guide preview
        # in the same publish.
        session = getattr(self._container, "session", None)
        if session is not None and getattr(session, "model", None) is not None:
            entry = getattr(session.dll, "Tonic_SetAmplifiedHair", None)
            if entry is not None:
                entry(session.model, 1 if value else 0)
                session.publish()
        self.refresh()

    # ---- warnings / status --------------------------------------------

    def _refreshWarnings(self, session, status=None):
        state = self._container.tonicState
        key = tonicHud.warningsKey(state, session, status)
        if key is not None and key == self._warningsKey:
            return
        if self._warningsKey is not None:
            # The key moves on every model version, which is every
            # publish, so the key alone still means three bounded ABI
            # reads (4 096 smoothness scores among them) per artist op.
            # The list is advisory -- uncovered faces, root crossings,
            # kink spikes -- and nobody reads it inside a stroke, so it
            # is re-read at the dock's own 250 ms cadence at most. A
            # forced refresh (key None, mode change, end of gesture)
            # still goes straight through.
            now = time.monotonic()
            if (now - self._warningsAt) * 1000.0 < REFRESH_MS:
                return
            self._warningsAt = now
        else:
            self._warningsAt = time.monotonic()
        self._warningsKey = key
        rows = tonicHud.warnings(state, session, status)
        texts = tuple("%s %s" % (_SEVERITY_PREFIX.get(row.severity, "[?]"),
                                 row.text) for row in rows)
        if texts == self._warningsTexts:
            # The model moved but said nothing new. Rebuilding the widget
            # here would drop and re-create a QListWidgetItem (and the
            # Python select-action closure it carries in UserRole) several
            # times per artist op for no visible change.
            return
        self._warningsTexts = texts
        self._warningsList.clear()
        for text, row in zip(texts, rows):
            item = QtWidgets.QListWidgetItem(text)
            item.setData(QtCore.Qt.UserRole, row.selectAction)
            self._warningsList.addItem(item)

    def _onWarningClicked(self, item):
        action = item.data(QtCore.Qt.UserRole)
        if action is not None:
            action(self._container)
            self.refresh()

    def _onBreadcrumbClicked(self, href):
        """A breadcrumb link: focus that level."""
        try:
            level = int(href)
        except (TypeError, ValueError):
            return
        viewport = getattr(self._container, "viewport", None)
        loop = (getattr(viewport, "loop", None)
                if viewport is not None else None)
        focus = getattr(loop, "focusLevel", None)
        if focus is not None:
            focus(level)
        else:
            tonicHierarchy.focusLevel(self._container.tonicState, level)
        self.refresh()

    def _breadcrumbHtml(self, state):
        return " &gt; ".join(
            '<a href="%d">%s</a>' % (level, _escape(label))
            for level, label in tonicHierarchy.breadcrumbSegments(state))

    def _refreshStatusStrip(self, session, status=None):
        state = self._container.tonicState
        strip = tonicHud.statusStrip(state, session, status)
        skew = (" [MAP BEHIND]" if strip.amber else "")
        ladder = int((status or {}).get("ladderStep", 0))
        crumbs = self._breadcrumbHtml(state) or "L1"
        text = ("| L%d | model v%d stage v%d pending v%d | "
               "map v%d baked v%d%s | swap %.1f ms | %s%s"
               % (strip.level, strip.modelVersion, strip.committedVersion,
                  strip.pendingVersion, strip.mapVersion, strip.bakedVersion,
                  skew, strip.lastSwapMs, strip.gpuText,
                  (" | fidelity step %d" % ladder) if ladder else ""))
        # setText re-lays the label out and schedules a repaint (and on a
        # rich-text label re-parses the HTML first); setPalette re-polishes
        # the widget. Neither is worth doing for a string that has not
        # moved, and at four refreshes per artist op most have not.
        if crumbs != self._crumbText:
            self._crumbText = crumbs
            self._statusLabel.setText(crumbs)
        if text != self._statusText:
            self._statusText = text
            self._statusDetail.setText(text)
        if strip.amber != self._statusAmber:
            self._statusAmber = strip.amber
            color = QtGui.QColor("#B8860B") if strip.amber else \
                self.palette().color(QtGui.QPalette.WindowText)
            for label in (self._statusLabel, self._statusDetail):
                palette = label.palette()
                palette.setColor(QtGui.QPalette.WindowText, color)
                label.setPalette(palette)
        amplified = bool(state.showAmplifiedHair)
        if amplified != self._amplifiedCheck.isChecked():
            self._amplifiedCheck.blockSignals(True)
            self._amplifiedCheck.setChecked(amplified)
            self._amplifiedCheck.blockSignals(False)

    # ---- refresh / timer --------------------------------------------------

    def _shelfSignature(self, state):
        attr = _SUBMODE_STATE_ATTR.get(self._activeMode)
        return (self._activeMode,
                getattr(state, attr, "") if attr else "")

    def refresh(self):
        """Re-read state/session and update every widget. Cheap enough to
        call after every publish (plan/18 section 3.5); also called on a
        250 ms timer while the dock is visible.

        "Cheap enough" is load-bearing and used not to be true: the TN-5
        controller soak measured four of these per artist op at 11 ms an
        op, which was most of the deferred work the V7 note could not
        name. Every part now re-reads only when its own inputs moved, and
        the one TonicSession.status() this takes is passed down instead of
        each reader building its own.
        """
        session = getattr(self._container, "session", None)
        state = self._container.tonicState
        # A number key switches the mode through the viewport controller,
        # so the dock has to notice a mode it did not set itself -- or it
        # goes on showing the previous mode's sub-mode shelf, parameter
        # rows and action buttons while the shelf button says otherwise.
        if state.activeMode and state.activeMode != self._activeMode:
            self._adoptMode(state.activeMode)
        elif (self._activeMode
              and int(getattr(state, "activeLevel", 0)) != self._pageLevel):
            # The only state the generated rows read is the active level,
            # and only for two labels in the hierarchy panel. Re-adopt on
            # a level change so those labels follow it; the page cache
            # reuses everything whose signature did not move.
            self._adoptMode(self._activeMode)
        shelfKey = self._shelfSignature(state)
        if shelfKey != self._shelfKey:
            self._shelfKey = shelfKey
            self._syncModeButtons()
            self._syncSubModeButtons()
        if getattr(session, "gestureActive", False):
            # Mid-stroke. The publish hook fires once per mouse sample, so
            # a six-sample drag would re-read the whole panel six times
            # while the artist is watching the viewport, not the dock.
            # Note it as stale instead: the release publishes again and
            # both the idle pump and the 250 ms timer refresh after it.
            self._staleContent = True
            return
        if self._staleContent:
            # Whatever the gesture changed has to be re-read even if the
            # keys below happen to match what the gesture started from.
            self._staleContent = False
            self._warningsKey = None
            self._statusText = None
            self._crumbText = None
        status = None
        readStatus = getattr(session, "status", None)
        if callable(readStatus):
            status = dict(readStatus() or {})
        self._refreshParams(session)
        self._refreshWarnings(session, status)
        self._refreshStatusStrip(session, status)

    def _onVisibilityChanged(self, visible):
        # workspaceOpen gates the V2 application-level hotkey filter (plan/18
        # section 3.4): the shelf's number/letter keys only fire while the
        # dock is actually visible, not merely constructed.
        self._container.tonicState.workspaceOpen = bool(visible)
        if visible:
            self._timer.start()
        else:
            self._timer.stop()

    def _onTimerTick(self):
        if self.isVisible():
            self.refresh()
