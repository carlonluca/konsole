/*
    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "ViewManager.h"

#include <config-konsole.h>

// Qt
#include <QStringList>
#include <QAction>

// KDE
#include <KAcceleratorManager>
#include <KLocalizedString>
#include <KActionCollection>
#include <KConfigGroup>

// Konsole
#include <windowadaptor.h>

#include "ColorScheme.h"
#include "ColorSchemeManager.h"
#include "Session.h"
#include "TerminalDisplay.h"
#include "SessionController.h"
#include "SessionManager.h"
#include "ProfileManager.h"
#include "ViewSplitter.h"
#include "Enumeration.h"
#include "ViewContainer.h"

using namespace Konsole;

int ViewManager::lastManagerId = 0;

ViewManager::ViewManager(QObject *parent, KActionCollection *collection) :
    QObject(parent),
    _viewSplitter(nullptr),
    _pluggedController(nullptr),
    _sessionMap(QHash<TerminalDisplay *, Session *>()),
    _actionCollection(collection),
    _navigationMethod(NoNavigation),
    _navigationVisibility(NavigationNotSet),
    _newTabBehavior(PutNewTabAtTheEnd),
    _managerId(0),
    _mtdManager(new MultiTerminalDisplayManager(this, this))
{
    // create main view area
    _viewSplitter = new ViewSplitter(nullptr);
    _viewSplitter->setStyleSheet(QStringLiteral("background-color:black;"));

    KAcceleratorManager::setNoAccel(_viewSplitter);

    // the ViewSplitter class supports both recursive and non-recursive splitting,
    // in non-recursive mode, all containers are inserted into the same top-level splitter
    // widget, and all the divider lines between the containers have the same orientation
    //
    // the ViewManager class is not currently able to handle a ViewSplitter in recursive-splitting
    // mode
    _viewSplitter->setRecursiveSplitting(false);
    _viewSplitter->setFocusPolicy(Qt::NoFocus);

    // setup actions which are related to the views
    setupActions();

    // emit a signal when all of the views held by this view manager are destroyed
    connect(_viewSplitter.data(), &Konsole::ViewSplitter::allContainersEmpty,
            this, &Konsole::ViewManager::empty);
    connect(_viewSplitter.data(), &Konsole::ViewSplitter::empty, this,
            &Konsole::ViewManager::empty);

    // listen for profile changes
    connect(ProfileManager::instance(), &Konsole::ProfileManager::profileChanged,
            this, &Konsole::ViewManager::profileChanged);
    connect(SessionManager::instance(), &Konsole::SessionManager::sessionUpdated,
            this, &Konsole::ViewManager::updateViewsForSession);

    //prepare DBus communication
    new WindowAdaptor(this);

    _managerId = ++lastManagerId;
    QDBusConnection::sessionBus().registerObject(QLatin1String("/Windows/")
                                                 + QString::number(_managerId), this);
}

ViewManager::~ViewManager() = default;

int ViewManager::managerId() const
{
    return _managerId;
}

QWidget *ViewManager::activeView() const
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    if (container != nullptr) {
        return container->currentWidget();
    } else {
        return nullptr;
    }
}

QWidget *ViewManager::widget() const
{
    return _viewSplitter;
}

void ViewManager::setupActions()
{
    Q_ASSERT(_actionCollection);
    if (_actionCollection == nullptr) {
        return;
    }

    KActionCollection *collection = _actionCollection;

    QAction *nextViewAction = new QAction(i18nc("@action Shortcut entry", "Next Tab"), this);
    QAction *previousViewAction = new QAction(i18nc("@action Shortcut entry", "Previous Tab"), this);
    QAction *lastViewAction = new QAction(i18nc("@action Shortcut entry",
                                                "Switch to Last Tab"), this);
    QAction *nextContainerAction = new QAction(i18nc("@action Shortcut entry",
                                                     "Next View Container"), this);

    QAction *moveViewLeftAction = new QAction(i18nc("@action Shortcut entry", "Move Tab Left"), this);
    QAction *moveViewRightAction = new QAction(i18nc("@action Shortcut entry",
                                                     "Move Tab Right"), this);

    // Crashes on Mac.
#if defined(ENABLE_DETACHING)
    QAction *detachViewAction = collection->addAction(QStringLiteral("detach-view"));
    detachViewAction->setEnabled(true);
    detachViewAction->setIcon(QIcon::fromTheme(QStringLiteral("tab-detach")));
    detachViewAction->setText(i18nc("@action:inmenu", "D&etach Current Tab"));
    // Ctrl+Shift+D is not used as a shortcut by default because it is too close
    // to Ctrl+D - which will terminate the session in many cases
    collection->setDefaultShortcut(detachViewAction, Konsole::ACCEL + Qt::SHIFT + Qt::Key_H);

    connect(this, &Konsole::ViewManager::splitViewToggle, this,
            &Konsole::ViewManager::updateDetachViewState);
    connect(detachViewAction, &QAction::triggered, this, &Konsole::ViewManager::detachActiveView);
#endif

    // Next / Previous View , Next Container
    collection->addAction(QStringLiteral("next-view"), nextViewAction);
    collection->addAction(QStringLiteral("previous-view"), previousViewAction);
    collection->addAction(QStringLiteral("last-tab"), lastViewAction);
    collection->addAction(QStringLiteral("next-container"), nextContainerAction);
    collection->addAction(QStringLiteral("move-view-left"), moveViewLeftAction);
    collection->addAction(QStringLiteral("move-view-right"), moveViewRightAction);

    // Switch to tab N shortcuts
    const int SWITCH_TO_TAB_COUNT = 19;
    for (int i = 0; i < SWITCH_TO_TAB_COUNT; i++) {
        QAction *switchToTabAction = new QAction(i18nc("@action Shortcut entry", "Switch to Tab %1", i + 1), this);

        connect(switchToTabAction, &QAction::triggered, this,
           [this, i]() {
               switchToView(i);
           });
        collection->addAction(QStringLiteral("switch-to-tab-%1").arg(i), switchToTabAction);
    }

    // Menu item for the vertical split of the multi terminal
    QAction* multiTerminalVerAction = new QAction(
                QIcon::fromTheme(QStringLiteral("view-split-left-right")),
                i18nc("@action:inmenu", "Split Pane &Vertically"), this);
    multiTerminalVerAction->setEnabled(true);
    multiTerminalVerAction->setShortcut(QKeySequence(Qt::META + Qt::Key_D));
    collection->addAction(QStringLiteral("multi-terminal-ver"), multiTerminalVerAction);
    _viewSplitter->addAction(multiTerminalVerAction);
    connect(multiTerminalVerAction, SIGNAL(triggered()), this, SLOT(multiTerminalVertical()));


    // Menu item for the horizontal split of the multi terminal
    QAction* multiTerminalHorAction = new QAction(
                QIcon::fromTheme(QStringLiteral("view-split-top-bottom")),
                i18nc("@action:inmenu", "Split Pane &Horizontally"), this);
    multiTerminalHorAction->setEnabled(true);
    multiTerminalHorAction->setShortcut(QKeySequence(Qt::META + Qt::CTRL + Qt::Key_D));
    collection->addAction(QStringLiteral("multi-terminal-hor"), multiTerminalHorAction);
    _viewSplitter->addAction(multiTerminalHorAction);
    connect(multiTerminalHorAction, SIGNAL(triggered()), this, SLOT(multiTerminalHorizontal()));


    // Menu item for closing a multi terminal
    QAction* closeMultiTerminalAction = new QAction(
                QIcon::fromTheme(QStringLiteral("view-close")),
                i18nc("@action:inmenu", "&Close"), this);
    closeMultiTerminalAction->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_W));
    collection->addAction(QStringLiteral("multi-terminal-close"), closeMultiTerminalAction);
    _viewSplitter->addAction(closeMultiTerminalAction);
    connect(closeMultiTerminalAction, SIGNAL(triggered()), this, SLOT(multiTerminalClose()));

    // Shortcut to move to the MTD to the left
    QAction* goToLeftMtdAction = collection->addAction(
                QStringLiteral("to-left-mtd"), this, SLOT(moveToLeftMtd()));
    goToLeftMtdAction->setText(i18n("&Move to closest multi-terminal on the left"));
    // TODO: icon?
    goToLeftMtdAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    goToLeftMtdAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_Left));
    // Shortcut to move to the MTD above
    QAction* goToTopMtdAction = collection->addAction(
                QStringLiteral("to-top-mtd"), this, SLOT(moveToTopMtd()));
    goToTopMtdAction->setText(i18n("&Move to closest multi-terminal above"));
    // TODO: icon?
    goToTopMtdAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    goToTopMtdAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_Up));
    // Shortcut to move to the MTD to the right
    QAction* goToRightMtdAction = collection->addAction(
                QStringLiteral("to-right-mtd"), this, SLOT(moveToRightMtd()));
    goToRightMtdAction->setText(i18n("&Move to closest multi-terminal on the right"));
    // TODO: icon?
    goToRightMtdAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    goToRightMtdAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_Right));
    // Shortcut to move to the MTD below
    QAction* goToBottomMtdAction = collection->addAction(
                QStringLiteral("to-bottom-mtd"), this, SLOT(moveToBottomMtd()));
    goToBottomMtdAction->setText(i18n("&Move to closest multi-terminal below"));
    // TODO: icon?
    goToBottomMtdAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-rename")));
    goToBottomMtdAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_Down));

    // keyboard shortcut only actions
    const QList<QKeySequence> nextViewActionKeys{Qt::SHIFT + Qt::Key_Right, Qt::CTRL + Qt::Key_PageDown};
    collection->setDefaultShortcuts(nextViewAction, nextViewActionKeys);
    connect(nextViewAction, &QAction::triggered, this, &Konsole::ViewManager::nextView);
    _viewSplitter->addAction(nextViewAction);

    const QList<QKeySequence> previousViewActionKeys{Qt::SHIFT + Qt::Key_Left, Qt::CTRL + Qt::Key_PageUp};
    collection->setDefaultShortcuts(previousViewAction, previousViewActionKeys);
    connect(previousViewAction, &QAction::triggered, this, &Konsole::ViewManager::previousView);
    _viewSplitter->addAction(previousViewAction);

    collection->setDefaultShortcut(nextContainerAction, Qt::SHIFT + Qt::Key_Tab);
    connect(nextContainerAction, &QAction::triggered, this, &Konsole::ViewManager::nextContainer);
    _viewSplitter->addAction(nextContainerAction);

#ifdef Q_OS_MACOS
    collection->setDefaultShortcut(moveViewLeftAction,
                                   Konsole::ACCEL + Qt::SHIFT + Qt::Key_BracketLeft);
#else
    collection->setDefaultShortcut(moveViewLeftAction, Konsole::ACCEL + Qt::SHIFT + Qt::Key_Left);
#endif
    connect(moveViewLeftAction, &QAction::triggered, this,
            &Konsole::ViewManager::moveActiveViewLeft);
    _viewSplitter->addAction(moveViewLeftAction);

#ifdef Q_OS_MACOS
    collection->setDefaultShortcut(moveViewRightAction,
                                   Konsole::ACCEL + Qt::SHIFT + Qt::Key_BracketRight);
#else
    collection->setDefaultShortcut(moveViewRightAction, Konsole::ACCEL + Qt::SHIFT + Qt::Key_Right);
#endif
    connect(moveViewRightAction, &QAction::triggered, this,
            &Konsole::ViewManager::moveActiveViewRight);
    _viewSplitter->addAction(moveViewRightAction);

    connect(lastViewAction, &QAction::triggered, this, &Konsole::ViewManager::lastView);
    _viewSplitter->addAction(lastViewAction);
}

void ViewManager::switchToView(int index)
{
    _viewSplitter->activeContainer()->setCurrentIndex(index);
}

void ViewManager::updateDetachViewState()
{
    Q_ASSERT(_actionCollection);
    if (_actionCollection == nullptr) {
        return;
    }

    const bool splitView = _viewSplitter->containers().count() >= 2;
    auto activeContainer = _viewSplitter->activeContainer();
    const bool shouldEnable = splitView
                              || ((activeContainer != nullptr)
                                  && activeContainer->count() >= 2);

    QAction *detachAction = _actionCollection->action(QStringLiteral("detach-view"));

    if ((detachAction != nullptr) && shouldEnable != detachAction->isEnabled()) {
        detachAction->setEnabled(shouldEnable);
    }
}

void ViewManager::moveActiveViewLeft()
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    Q_ASSERT(container);
    container->moveActiveView(TabbedViewContainer::MoveViewLeft);
}

void ViewManager::moveActiveViewRight()
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    Q_ASSERT(container);
    container->moveActiveView(TabbedViewContainer::MoveViewRight);
}

void ViewManager::nextContainer()
{
    _viewSplitter->activateNextContainer();
}

void ViewManager::nextView()
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    Q_ASSERT(container);
    container->activateNextView();
}

void ViewManager::previousView()
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    Q_ASSERT(container);
    container->activatePreviousView();
}

void ViewManager::lastView()
{
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    Q_ASSERT(container);
    container->activateLastView();
}

void ViewManager::detachActiveView()
{
    // find the currently active view and remove it from its container
    TabbedViewContainer *container = _viewSplitter->activeContainer();
    detachView(container, container->currentWidget());
}

void ViewManager::detachView(TabbedViewContainer *container, QWidget *view)
{
#if !defined(ENABLE_DETACHING)
    return;
#endif

    MultiTerminalDisplay * viewToDetach = qobject_cast<MultiTerminalDisplay*>(view);

    if (viewToDetach == nullptr)
        return;

    QSet<TerminalDisplay*> tds = _mtdManager->getTerminalDisplaysOfContainer(viewToDetach);
    foreach (TerminalDisplay* td, tds) {
        // Every time this signal is emitted, a new window with the given session is created
        // see Application::detachView(Session* session)
        // Also a new ViewManager will be created, how to clone the multi-terminals?

        // BR390736 - some instances are sending invalid session to viewDetached()
        Session *sessionToDetach = _sessionMap[td];
        if (sessionToDetach == nullptr)
            return;
        emit viewDetached(sessionToDetach);
        _sessionMap.remove(td);
    }

    // remove the view from this window
    container->removeView(viewToDetach);
    viewToDetach->deleteLater();

    // if the container from which the view was removed is now empty then it can be deleted,
    // unless it is the only container in the window, in which case it is left empty
    // so that there is always an active container
    if (_viewSplitter->containers().count() > 1
        && container->count() == 0) {
        removeContainer(container);
    }
}

void ViewManager::sessionFinished()
{
    // if this slot is called after the view manager's main widget
    // has been destroyed, do nothing
    if (_viewSplitter.isNull()) {
        return;
    }

    auto *session = qobject_cast<Session *>(sender());
    Q_ASSERT(session);

    // TODO: all multi terminals must be removed as well

    // close attached views
    QList<TerminalDisplay *> children = _viewSplitter->findChildren<TerminalDisplay *>();

    foreach (TerminalDisplay *view, children) {
        if (_sessionMap[view] == session) {
            _sessionMap.remove(view);
            view->deleteLater();
        }
    }

    // Only remove the controller from factory() if it's actually controlling
    // the session from the sender.
    // This fixes BUG: 348478 - messed up menus after a detached tab is closed
    if ((!_pluggedController.isNull()) && (_pluggedController->session() == session)) {
        // This is needed to remove this controller from factory() in
        // order to prevent BUG: 185466 - disappearing menu popup
        emit unplugController(_pluggedController);
    }
}

void ViewManager::viewActivated(QWidget *view)
{
    Q_ASSERT(view != nullptr);

    // focus the activated view, this will cause the SessionController
    // to notify the world that the view has been focused and the appropriate UI
    // actions will be plugged in.
    view->setFocus(Qt::OtherFocusReason);
}

void ViewManager::splitLeftRight()
{
    splitView(Qt::Horizontal);
}

void ViewManager::splitTopBottom()
{
    splitView(Qt::Vertical);
}

void ViewManager::splitView(Qt::Orientation orientation)
{
    TabbedViewContainer *container = createContainer();

    // iterate over each session which has a view in the current active
    // container and create a new view for that session in a new container
    // TODO XXX: non va bene: prima era _viewSplitter->activeContainer()->views() che
    //       per un tab container significava tutte le tabs (1 tab per TerminalDisplay).
    //       Ora sono i TerminalDisplay in una singola tab.
    // foreach(QWidget* view,  getTerminalsFromContainer(_viewSplitter->activeContainer())) {


    // For each view of the container (for each tab): _viewSplitter->activeContainer()->views()
    // Get the tree of MTDs of that Tab
    // Create a widget that contains all the subwidgets (the MTD tree) but uses the same terminal sessions
    // Add this widget (i.e., tab) to the new container (created above)

    TabbedViewContainer* activeContainer = _viewSplitter->activeContainer();
    for (int i = 0; i < activeContainer->count(); i++) {
        MultiTerminalDisplay* mtd = qobject_cast<MultiTerminalDisplay*>(activeContainer->widget(i));
        if (!mtd) {
            qCritical() << "Cannot cast container view to MultiTerminalDisplay";
            return;
        }

        _mtdManager->cloneMtd(mtd, container);
    }

    _viewSplitter->addContainer(container, orientation);
    emit splitViewToggle(_viewSplitter->containers().count() > 0);

    // focus the new container
    container->currentWidget()->setFocus();

    // ensure that the active view is focused after the split / unsplit
    activeContainer = _viewSplitter->activeContainer();
    QWidget *activeView = activeContainer != nullptr ? activeContainer->currentWidget() : nullptr;

    if (activeView != nullptr)
        activeView->setFocus(Qt::OtherFocusReason);
}

void ViewManager::removeContainer(TabbedViewContainer *container)
{
    // lcarlon: not sure why this was deleted
#if 0
    // remove session map entries for views in this container
    for(int i = 0, end = container->count(); i < end; i++) {
        auto view = container->widget(i);
        auto *display = qobject_cast<TerminalDisplay *>(view);
        Q_ASSERT(display);
        _sessionMap.remove(display);
    }

    _viewSplitter->removeContainer(container);
    container->deleteLater();

    emit splitViewToggle(_viewSplitter->containers().count() > 1);
#endif
}

void ViewManager::multiTerminalHorizontal()
{
    // This is called from the menu action
    qDebug() << "ViewManager::multiTerminalHorizontal() ";
    createMultiTerminalView(Qt::Vertical);
}
void ViewManager::multiTerminalVertical()
{
    // This is called from the menu action
    qDebug() << "ViewManager::multiTerminalVertical() ";
    createMultiTerminalView(Qt::Horizontal);
}

void ViewManager::multiTerminalClose()
{
    // TODO: make sure we close the one that has focused.

    MultiTerminalDisplay* containerMtd = qobject_cast<MultiTerminalDisplay*>(_viewSplitter->activeContainer()->currentWidget());

    // MultiTerminalDisplay with focus
    MultiTerminalDisplay* mtd = _mtdManager->getFocusedMultiTerminalDisplay(containerMtd);
    MultiTerminalDisplay* root = _mtdManager->getRootNode(containerMtd);

    _mtdManager->removeTerminalDisplay(mtd);

//     // TODO: not ok, this closes the application even if other tabs are open (mtdtrees with different roots)
//     if (_mtdManager->getNumberOfNodes(root) == 0) {
//         // no more terminal, let's exit()
//         exit(0);
//     }

    //_mtdManager->dismissMultiTerminals(mtd);
}

void ViewManager::moveToLeftMtd()
{
    moveMtdFocus(MultiTerminalDisplayManager::LEFT);
}

void ViewManager::moveToTopMtd()
{
    moveMtdFocus(MultiTerminalDisplayManager::TOP);
}

void ViewManager::moveToRightMtd()
{
    moveMtdFocus(MultiTerminalDisplayManager::RIGHT);
}

void ViewManager::moveToBottomMtd()
{
    moveMtdFocus(MultiTerminalDisplayManager::BOTTOM);
}

void ViewManager::moveMtdFocus(MultiTerminalDisplayManager::Directions direction)
{
    MultiTerminalDisplay* containerMtd = qobject_cast<MultiTerminalDisplay*>(_viewSplitter->activeContainer()->currentWidget());
    MultiTerminalDisplay* focusMtd = _mtdManager->getFocusedMultiTerminalDisplay(containerMtd);
    TerminalDisplay* td = _mtdManager->getTerminalDisplayTo(focusMtd, direction, containerMtd);
    if (td) {
        td->setFocus();
    }
}

void ViewManager::expandActiveContainer()
{
    _viewSplitter->adjustContainerSize(_viewSplitter->activeContainer(), 10);
}

void ViewManager::shrinkActiveContainer()
{
    _viewSplitter->adjustContainerSize(_viewSplitter->activeContainer(), -10);
}

void ViewManager::closeActiveContainer()
{
    // only do something if there is more than one container active
    if (_viewSplitter->containers().count() > 1) {
        TabbedViewContainer *container = _viewSplitter->activeContainer();

        removeContainer(container);

        // focus next container so that user can continue typing
        // without having to manually focus it themselves
        nextContainer();
    }
}

void ViewManager::closeOtherContainers()
{
    TabbedViewContainer *active = _viewSplitter->activeContainer();

    foreach (TabbedViewContainer *container, _viewSplitter->containers()) {
        if (container != active) {
            removeContainer(container);
        }
    }
}

SessionController *ViewManager::createController(Session *session, TerminalDisplay *view)
{
    // create a new controller for the session, and ensure that this view manager
    // is notified when the view gains the focus
    auto controller = new SessionController(session, view, this);
    connect(controller, &Konsole::SessionController::focused, this,
            &Konsole::ViewManager::controllerChanged);
    connect(session, &Konsole::Session::destroyed, controller,
            &Konsole::SessionController::deleteLater);
    connect(session, &Konsole::Session::primaryScreenInUse, controller,
            &Konsole::SessionController::setupPrimaryScreenSpecificActions);
    connect(session, &Konsole::Session::selectionChanged, controller,
            &Konsole::SessionController::selectionChanged);
    connect(view, &Konsole::TerminalDisplay::destroyed, controller,
            &Konsole::SessionController::deleteLater);

    // if this is the first controller created then set it as the active controller
    if (_pluggedController.isNull()) {
        controllerChanged(controller);
    }

    return controller;
}

void ViewManager::controllerChanged(SessionController *controller)
{
    if (controller == _pluggedController) {
        return;
    }

    _viewSplitter->setFocusProxy(controller->view());

    _pluggedController = controller;
    emit activeViewChanged(controller);
}

SessionController *ViewManager::activeViewController() const
{
    return _pluggedController;
}

TerminalDisplay* ViewManager::createAndSetupTerminalDisplay(Session* session)
{
    if (!session) {qCritical() << "session was null!"; return nullptr;}
    TerminalDisplay* td = createTerminalDisplay(session);
    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);
    applyProfileToView(td, profile);
    createController(session, td);
    _sessionMap[td] = session;
    session->addView(td);
    return td;
}

void ViewManager::createView(Session *session, TabbedViewContainer *container, int index)
{
    // notify this view manager when the session finishes so that its view
    // can be deleted
    //
    // Use Qt::UniqueConnection to avoid duplicate connection
    connect(session, &Konsole::Session::finished, this, &Konsole::ViewManager::sessionFinished,
            Qt::UniqueConnection);

    TerminalDisplay *display = createTerminalDisplay(session);

    // TODO: container here is the container of tabs, while what we want is the view of a single tab
    // Maybe it is container->activeView(); ?
    MultiTerminalDisplay* multiTerminalDisplay = _mtdManager->createRootTerminalDisplay(display, session, container);

    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);
    applyProfileToView(display, profile);

    // set initial size
    const QSize &preferredSize = session->preferredSize();

    display->setSize(preferredSize.width(), preferredSize.height());
    ViewProperties *properties = createController(session, display);

    _sessionMap[display] = session;
    container->addView(multiTerminalDisplay, properties, index);
    session->addView(display);

    // tell the session whether it has a light or dark background
    session->setDarkBackground(colorSchemeForProfile(profile)->hasDarkBackground());

    if (container == _viewSplitter->activeContainer()) {
        container->setCurrentWidget(multiTerminalDisplay);
        display->setFocus(Qt::OtherFocusReason);
    }

    updateDetachViewState();
}

void ViewManager::createView(Session *session)
{
    qInfo() << "lcarlon:" << Q_FUNC_INFO;

    // create the default container
    if (_viewSplitter->containers().count() == 0) {
        qDebug() << "lcarlon: adding container";
        TabbedViewContainer *container = createContainer();
        _viewSplitter->addContainer(container, Qt::Vertical);
    }

    // new tab will be put at the end by default.
    int index = -1;

    if (_newTabBehavior == PutNewTabAfterCurrentTab) {
        index = _viewSplitter->activeContainer()->currentIndex() + 1;
    }

    // iterate over the view containers owned by this view manager
    // and create a new terminal display for the session in each of them, along with
    // a controller for the session/display pair
    // TODO: this means that if a view manager is split into two view containers and a new
    // tab is created, the tab will go in both views. So the same must happen for multi
    // terminal
    foreach (TabbedViewContainer *container, _viewSplitter->containers()) {
        qInfo() << "lcarlon: create view in container";
        createView(session, container, index);
    }
}

void ViewManager::createMultiTerminalView(Qt::Orientation orientation)
{
    qDebug() << "ViewManager::createMultiTerminalView";

    QString currentWorkingDir = activeViewController()->currentDir();

    Profile::Ptr defaultProfile = ProfileManager::instance()->defaultProfile();

    Session* session = SessionManager::instance()->createSession(defaultProfile);

    if (!currentWorkingDir.isEmpty() && defaultProfile->startInCurrentSessionDir())
        session->setInitialWorkingDirectory(currentWorkingDir);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    connect(session, SIGNAL(finished()), this, SLOT(sessionFinished()), Qt::UniqueConnection);

    TerminalDisplay* display = createTerminalDisplay(session);
    if (!session) {qCritical() << "session was null!"; return;}
    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);
    applyProfileToView(display, profile);
    _sessionMap[display] = session;
    session->addView(display);
    createController(session, display);

    MultiTerminalDisplay* containerMtd = qobject_cast<MultiTerminalDisplay*>(_viewSplitter->activeContainer()->currentWidget());
    MultiTerminalDisplay* multiTerminalDisplay = _mtdManager->getFocusedMultiTerminalDisplay(containerMtd);
    _mtdManager->addTerminalDisplay(display, session, multiTerminalDisplay, orientation);


    session->setDarkBackground(colorSchemeForProfile(profile)->hasDarkBackground());

    updateDetachViewState();
}

TabbedViewContainer *ViewManager::createContainer()
{

    auto *container = new TabbedViewContainer(this, _viewSplitter);
    container->setNavigationVisibility(_navigationVisibility);
    //TODO: Fix Detaching.
    connect(container, &TabbedViewContainer::detachTab, this, &ViewManager::detachView);

    // connect signals and slots
    connect(container, &Konsole::TabbedViewContainer::viewAdded, this,
           [this, container]() {
               containerViewsChanged(container);
           });

    connect(container, &Konsole::TabbedViewContainer::viewRemoved, this,
           [this, container]() {
               containerViewsChanged(container);
           });

    connect(container,
            static_cast<void (TabbedViewContainer::*)()>(&Konsole::TabbedViewContainer::newViewRequest), this,
            static_cast<void (ViewManager::*)()>(&Konsole::ViewManager::newViewRequest));
    connect(container,
            static_cast<void (TabbedViewContainer::*)(Profile::Ptr)>(&Konsole::TabbedViewContainer::newViewRequest),
            this,
            static_cast<void (ViewManager::*)(Profile::Ptr)>(&Konsole::ViewManager::newViewRequest));
    connect(container, &Konsole::TabbedViewContainer::moveViewRequest, this,
            &Konsole::ViewManager::containerMoveViewRequest);
    connect(container, &Konsole::TabbedViewContainer::viewRemoved, this,
            &Konsole::ViewManager::viewDestroyed);
    connect(container, &Konsole::TabbedViewContainer::activeViewChanged, this,
            &Konsole::ViewManager::viewActivated);

    return container;
}

void ViewManager::setNavigationMethod(NavigationMethod method)
{
    Q_ASSERT(_actionCollection);
    if (_actionCollection == nullptr) {
        return;
    }
    KActionCollection *collection = _actionCollection;

    _navigationMethod = method;

    // FIXME: The following disables certain actions for the KPart that it
    // doesn't actually have a use for, to avoid polluting the action/shortcut
    // namespace of an application using the KPart (otherwise, a shortcut may
    // be in use twice, and the user gets to see an "ambiguous shortcut over-
    // load" error dialog). However, this approach sucks - it's the inverse of
    // what it should be. Rather than disabling actions not used by the KPart,
    // a method should be devised to only enable those that are used, perhaps
    // by using a separate action collection.

    const bool enable = (method != NoNavigation);

    auto enableAction = [&enable, &collection](const QString& actionName) {
        auto *action = collection->action(actionName);
        if (action != nullptr) {
            action->setEnabled(enable);
        }
    };

    enableAction(QStringLiteral("next-view"));
    enableAction(QStringLiteral("previous-view"));
    enableAction(QStringLiteral("last-tab"));
    enableAction(QStringLiteral("split-view-left-right"));
    enableAction(QStringLiteral("split-view-top-bottom"));
    enableAction(QStringLiteral("rename-session"));
    enableAction(QStringLiteral("move-view-left"));
    enableAction(QStringLiteral("move-view-right"));
    enableAction(QStringLiteral("multi-terminal"));
}

void ViewManager::containerMoveViewRequest(int index, int id, TabbedViewContainer* sourceTabbedContainer)
{
#warning Complete this
#if 0
    ViewContainer* container = qobject_cast<ViewContainer*>(sender());
    SessionController* controller = qobject_cast<SessionController*>(ViewProperties::propertiesById(id));

    if (!controller)
        return;

    // do not move the last tab in a split view.
    if (sourceTabbedContainer) {
        QPointer<ViewContainer> sourceContainer = qobject_cast<ViewContainer*>(sourceTabbedContainer);

        if (_viewSplitter->containers().contains(sourceContainer)) {
            return;
        } else {
            ViewManager* sourceViewManager = sourceTabbedContainer->connectedViewManager();

            // do not remove the last tab on the window
            if (qobject_cast<ViewSplitter*>(sourceViewManager->widget())->containers().size() > 1) {
                return;
            }
        }
    }

    createView(controller->session(), container, index);
    controller->session()->refresh();
#endif
}

ViewManager::NavigationMethod ViewManager::navigationMethod() const
{
    return _navigationMethod;
}

void ViewManager::containerViewsChanged(TabbedViewContainer *container)
{
    if ((!_viewSplitter.isNull()) && container == _viewSplitter->activeContainer()) {
        emit viewPropertiesChanged(viewProperties());
    }
}

void ViewManager::viewDestroyed(QWidget *view)
{
    // Note: the received QWidget has already been destroyed, so
    // using dynamic_cast<> or qobject_cast<> does not work here
    // We only need the pointer address to look it up below
    auto *display = reinterpret_cast<TerminalDisplay *>(view);

    // 1. detach view from session
    // 2. if the session has no views left, close it
    Session *session = _sessionMap[ display ];
    _sessionMap.remove(display);
    if (session != nullptr) {
        if (session->views().count() == 0) {
            session->close();
        }
    }
    //we only update the focus if the splitter is still alive
    if (!_viewSplitter.isNull()) {
        updateDetachViewState();
    }
    // The below causes the menus  to be messed up
    // Only happens when using the tab bar close button
//    if (_pluggedController)
//        emit unplugController(_pluggedController);
}

TerminalDisplay *ViewManager::createTerminalDisplay(Session *session)
{
    auto display = new TerminalDisplay(nullptr);
    display->setRandomSeed(session->sessionId() * 31);

    return display;
}

const ColorScheme *ViewManager::colorSchemeForProfile(const Profile::Ptr profile)
{
    const ColorScheme *colorScheme = ColorSchemeManager::instance()->
                                     findColorScheme(profile->colorScheme());
    if (colorScheme == nullptr) {
        colorScheme = ColorSchemeManager::instance()->defaultColorScheme();
    }
    Q_ASSERT(colorScheme);

    return colorScheme;
}

bool ViewManager::profileHasBlurEnabled(const Profile::Ptr profile)
{
    return colorSchemeForProfile(profile)->blur();
}

void ViewManager::applyProfileToView(TerminalDisplay *view, const Profile::Ptr profile)
{
    Q_ASSERT(profile);

    emit updateWindowIcon();

    // load color scheme
    ColorEntry table[TABLE_COLORS];
    const ColorScheme *colorScheme = colorSchemeForProfile(profile);
    colorScheme->getColorTable(table, view->randomSeed());
    view->setColorTable(table);
    view->setOpacity(colorScheme->opacity());
    view->setWallpaper(colorScheme->wallpaper());

    emit blurSettingChanged(colorScheme->blur());

    // load font
    view->setAntialias(profile->antiAliasFonts());
    view->setBoldIntense(profile->boldIntense());
    view->setUseFontLineCharacters(profile->useFontLineCharacters());
    view->setVTFont(profile->font());

    // set scroll-bar position
    view->setScrollBarPosition(Enum::ScrollBarPositionEnum(profile->property<int>(Profile::ScrollBarPosition)));
    view->setScrollFullPage(profile->property<bool>(Profile::ScrollFullPage));

    // show hint about terminal size after resizing
    view->setShowTerminalSizeHint(profile->showTerminalSizeHint());
    view->setDimWhenInactive(profile->dimWhenInactive());

    // terminal features
    view->setBlinkingCursorEnabled(profile->blinkingCursorEnabled());
    view->setBlinkingTextEnabled(profile->blinkingTextEnabled());
    view->setTripleClickMode(Enum::TripleClickModeEnum(profile->property<int>(Profile::TripleClickMode)));
    view->setAutoCopySelectedText(profile->autoCopySelectedText());
    view->setControlDrag(profile->property<bool>(Profile::CtrlRequiredForDrag));
    view->setDropUrlsAsText(profile->property<bool>(Profile::DropUrlsAsText));
    view->setBidiEnabled(profile->bidiRenderingEnabled());
    view->setLineSpacing(profile->lineSpacing());
    view->setTrimLeadingSpaces(profile->property<bool>(Profile::TrimLeadingSpacesInSelectedText));
    view->setTrimTrailingSpaces(profile->property<bool>(Profile::TrimTrailingSpacesInSelectedText));
    view->setOpenLinksByDirectClick(profile->property<bool>(Profile::OpenLinksByDirectClickEnabled));
    view->setUrlHintsModifiers(profile->property<int>(Profile::UrlHintsModifiers));
    view->setReverseUrlHintsEnabled(profile->property<int>(Profile::ReverseUrlHints));
    view->setMiddleClickPasteMode(Enum::MiddleClickPasteModeEnum(profile->property<int>(Profile::MiddleClickPasteMode)));
    view->setCopyTextAsHTML(profile->property<bool>(Profile::CopyTextAsHTML));

    // margin/center
    view->setMargin(profile->property<int>(Profile::TerminalMargin));
    view->setCenterContents(profile->property<bool>(Profile::TerminalCenter));

    // cursor shape
    view->setKeyboardCursorShape(Enum::CursorShapeEnum(profile->property<int>(Profile::CursorShape)));

    // cursor color
    // an invalid QColor is used to inform the view widget to
    // draw the cursor using the default color( matching the text)
    view->setKeyboardCursorColor(profile->useCustomCursorColor() ? profile->customCursorColor() : QColor());

    // word characters
    view->setWordCharacters(profile->wordCharacters());

    // bell mode
    view->setBellMode(profile->property<int>(Profile::BellMode));

    // mouse wheel zoom
    view->setMouseWheelZoom(profile->mouseWheelZoomEnabled());
    view->setAlternateScrolling(profile->property<bool>(Profile::AlternateScrolling));
}

void ViewManager::updateViewsForSession(Session *session)
{
    const Profile::Ptr profile = SessionManager::instance()->sessionProfile(session);

    const QList<TerminalDisplay *> sessionMapKeys = _sessionMap.keys(session);
    foreach (TerminalDisplay *view, sessionMapKeys) {
        applyProfileToView(view, profile);
    }
}

void ViewManager::profileChanged(Profile::Ptr profile)
{
    // update all views associated with this profile
    QHashIterator<TerminalDisplay *, Session *> iter(_sessionMap);
    while (iter.hasNext()) {
        iter.next();

        // if session uses this profile, update the display
        if (iter.key() != nullptr
            && iter.value() != nullptr
            && SessionManager::instance()->sessionProfile(iter.value()) == profile) {
            applyProfileToView(iter.key(), profile);
        }
    }
}

QList<ViewProperties *> ViewManager::viewProperties() const
{
    QList<ViewProperties *> list;

    TabbedViewContainer *container = _viewSplitter->activeContainer();
    if (container == nullptr) {
        return {};
    }

    list.reserve(container->count());

#warning Implementation missing
#if 0
    foreach (TerminalDisplay* view, _mtdManager->getTerminalDisplays()) {
        ViewProperties *properties = container->viewProperties(view);
        Q_ASSERT(properties);
        list << properties;
    }
#endif

    return list;
}

void ViewManager::saveSessions(KConfigGroup &group)
{
    // find all unique session restore IDs
    QList<int> ids;
    QSet<Session *> unique;
    int tab = 1;

    TabbedViewContainer *container = _viewSplitter->activeContainer();

    // first: sessions in the active container, preserving the order
    Q_ASSERT(container);
    if (container == nullptr) {
        return;
    }
    ids.reserve(container->count());

#warning Complete
#if 0
    auto *activeview = qobject_cast<TerminalDisplay *>(container->currentWidget());
    for (int i = 0, end = container->count(); i < end; i++) {
        auto *view = qobject_cast<TerminalDisplay *>(container->widget(i));
        Q_ASSERT(view);

        Session *session = _sessionMap[view];
        ids << SessionManager::instance()->getRestoreId(session);
        unique.insert(session);
        if (view == activeview) {
            group.writeEntry("Active", tab);
        }
        tab++;
    }

    // second: all other sessions, in random order
    // we don't want to have sessions restored that are not connected
    foreach (Session *session, _sessionMap) {
        if (!unique.contains(session)) {
            ids << SessionManager::instance()->getRestoreId(session);
            unique.insert(session);
        }
    }

    group.writeEntry("Sessions", ids);
#endif
}

void ViewManager::restoreSessions(const KConfigGroup &group)
{
    QList<int> ids = group.readEntry("Sessions", QList<int>());
    int activeTab = group.readEntry("Active", 0);
    TerminalDisplay *display = nullptr;

    int tab = 1;
    foreach (int id, ids) {
        Session *session = SessionManager::instance()->idToSession(id);

        if (session == nullptr) {
            qWarning() << "Unable to load session with id" << id;
            // Force a creation of a default session below
            ids.clear();
            break;
        }

        createView(session);
        if (!session->isRunning()) {
            session->run();
        }
        if (tab++ == activeTab) {
            display = qobject_cast<TerminalDisplay *>(activeView());
        }
    }

    if (display != nullptr) {
        _viewSplitter->activeContainer()->setCurrentWidget(display);
        display->setFocus(Qt::OtherFocusReason);
    }

    if (ids.isEmpty()) { // Session file is unusable, start default Profile
        Profile::Ptr profile = ProfileManager::instance()->defaultProfile();
        Session *session = SessionManager::instance()->createSession(profile);
        createView(session);
        if (!session->isRunning()) {
            session->run();
        }
    }
}

int ViewManager::sessionCount()
{
    return _sessionMap.size();
}

QStringList ViewManager::sessionList()
{
    QStringList ids;

    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        ids.append(QString::number(i.value()->sessionId()));
    }

    return ids;
}

int ViewManager::currentSession()
{
    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        if (i.key()->isVisible()) {
            return i.value()->sessionId();
        }
    }
    return -1;
}

void ViewManager::setCurrentSession(int sessionId)
{
    QHash<TerminalDisplay *, Session *>::const_iterator i;
    for (i = _sessionMap.constBegin(); i != _sessionMap.constEnd(); ++i) {
        if (i.value()->sessionId() == sessionId) {
            TabbedViewContainer *container = _viewSplitter->activeContainer();
            if (container != nullptr) {
                container->setCurrentWidget(i.key());
            }
        }
    }
}

int ViewManager::newSession()
{
    Profile::Ptr profile = ProfileManager::instance()->defaultProfile();
    Session *session = SessionManager::instance()->createSession(profile);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

int ViewManager::newSession(const QString &profile)
{
    const QList<Profile::Ptr> profilelist = ProfileManager::instance()->allProfiles();
    Profile::Ptr profileptr = ProfileManager::instance()->defaultProfile();

    for (const auto &i : profilelist) {
        if (i->name() == profile) {
            profileptr = i;
            break;
        }
    }

    Session *session = SessionManager::instance()->createSession(profileptr);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

int ViewManager::newSession(const QString &profile, const QString &directory)
{
    const QList<Profile::Ptr> profilelist = ProfileManager::instance()->allProfiles();
    Profile::Ptr profileptr = ProfileManager::instance()->defaultProfile();

    for (const auto &i : profilelist) {
        if (i->name() == profile) {
            profileptr = i;
            break;
        }
    }

    Session *session = SessionManager::instance()->createSession(profileptr);
    session->setInitialWorkingDirectory(directory);

    session->addEnvironmentEntry(QStringLiteral("KONSOLE_DBUS_WINDOW=/Windows/%1").arg(managerId()));

    createView(session);
    session->run();

    return session->sessionId();
}

QString ViewManager::defaultProfile()
{
    return ProfileManager::instance()->defaultProfile()->name();
}

QStringList ViewManager::profileList()
{
    return ProfileManager::instance()->availableProfileNames();
}

void ViewManager::nextSession()
{
    nextView();
}

void ViewManager::prevSession()
{
    previousView();
}

void ViewManager::moveSessionLeft()
{
    moveActiveViewLeft();
}

void ViewManager::moveSessionRight()
{
    moveActiveViewRight();
}

void ViewManager::setTabWidthToText(bool setTabWidthToText)
{
    for(auto container : _viewSplitter->containers()) {
        container->tabBar()->setExpanding(!setTabWidthToText);
        container->tabBar()->update();
    }
}

void ViewManager::setNavigationVisibility(NavigationVisibility navigationVisibility) {
    if (_navigationVisibility != navigationVisibility) {
        _navigationVisibility = navigationVisibility;
        for(auto *container : _viewSplitter->containers()) {
            container->setNavigationVisibility(navigationVisibility);
        }
    }
}

// TODO: remove this...
QList<TerminalDisplay*> ViewManager::getTerminalsFromContainer(TabbedViewContainer *container) const
{
    return _mtdManager->getTerminalDisplays();
}

void ViewManager::setNavigationBehavior(int behavior)
{
    _newTabBehavior = static_cast<NewTabBehavior>(behavior);
}
