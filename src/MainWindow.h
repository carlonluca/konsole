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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// Qt
#include <QPointer>
#include <QUrl>

// KDE
#include <KXmlGuiWindow>

// Konsole
#include "Profile.h"

class QAction;
class KActionMenu;
class KToggleAction;

namespace Konsole {
class IncrementalSearchBar;
class ViewManager;
class ViewProperties;
class Session;
class SessionController;
class ProfileList;
class BookmarkHandler;

/**
 * The main window.  This contains the menus and an area which contains the terminal displays.
 *
 * The main window does not create the views or the container widgets which hold the views.
 * This is done by the ViewManager class.  When a MainWindow is instantiated, it creates
 * a new ViewManager.  The ViewManager can then be used to create new terminal displays
 * inside the window.
 *
 * Do not construct new main windows directly, use Application's newMainWindow() method.
 */
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    /**
     * Constructs a new main window.  Do not create new main windows directly, use Application's
     * newMainWindow() method instead.
     */
    MainWindow();

    /**
     * Returns the view manager associated with this window.  The view manager can be used to
     * create new views on particular session objects inside this window.
     */
    ViewManager *viewManager() const;

    /**
     * Create a new session.
     *
     * @param profile The profile to use to create the new session.
     * @param directory Initial working directory for the new session or empty
     * if the default working directory associated with the profile should be used.
     */
    Session *createSession(Profile::Ptr profile, const QString &directory);

    /**
     * create a new SSH session.
     *
     * @param profile The profile to use to create the new session.
     * @param url the URL representing the new SSH connection
     */
    Session *createSSHSession(Profile::Ptr profile, const QUrl &url);

    /**
     * Helper method to make this window get input focus
     */
    void setFocus();

    /**
     * Set the initial visibility of the menubar.
     */
    void setMenuBarInitialVisibility(bool visible);

Q_SIGNALS:

    /**
     * Emitted by the main window to request the creation of a
     * new session in a new window.
     *
     * @param profile The profile to use to create the
     * first session in the new window.
     * @param directory Initial working directory for the new window or empty
     * if the default working directory associated with the profile should
     * be used.
     */
    void newWindowRequest(Profile::Ptr profile, const QString &directory);

    /**
     * Emitted when a view for one session is detached from this window
     */
    void viewDetached(Session *session);

protected:
    // Reimplemented for internal reasons.
    void showEvent(QShowEvent *event) Q_DECL_OVERRIDE;
    bool eventFilter(QObject *obj, QEvent *event) Q_DECL_OVERRIDE;

    // reimplemented from KMainWindow
    bool queryClose() Q_DECL_OVERRIDE;
    void saveProperties(KConfigGroup &group) Q_DECL_OVERRIDE;
    void readProperties(const KConfigGroup &group) Q_DECL_OVERRIDE;
    void saveGlobalProperties(KConfig *config) Q_DECL_OVERRIDE;
    void readGlobalProperties(KConfig *config) Q_DECL_OVERRIDE;

    // reimplemented from QWidget
    bool focusNextPrevChild(bool next) Q_DECL_OVERRIDE;

private Q_SLOTS:
    void newTab();
    void cloneTab();
    void closeTab();
    void newWindow();
    void showManageProfilesDialog();
    void activateMenuBar();
    void showSettingsDialog(const bool showProfilePage = false);
    void showShortcutsDialog();
    void newFromProfile(Profile::Ptr profile);
    void activeViewChanged(SessionController *controller);
    void disconnectController(SessionController *controller);
    void activeViewTitleChanged(ViewProperties *);

    void profileListChanged(const QList<QAction *> &sessionActions);
    void configureNotifications();
    void setBlur(bool blur);

    void updateWindowIcon();
    void updateWindowCaption();
    void openUrls(const QList<QUrl> &urls);

    // Sets the list of profiles to be displayed under the "New Tab" action
    void setProfileList(ProfileList *list);

    void applyKonsoleSettings();

    void updateUseTransparency();

public Q_SLOTS:
    void viewFullScreen(bool fullScreen);

private:
    void correctStandardShortcuts();
    void rememberMenuAccelerators();
    void removeMenuAccelerators();
    void restoreMenuAccelerators();
    void setupActions();
    QString activeSessionDir() const;
    void triggerAction(const QString &name) const;

    /**
     * Returns the bookmark handler associated with this window.
     */
    BookmarkHandler *bookmarkHandler() const;

    // sets the active shortcuts of actions in 'dest' to the shortcuts of actions
    // with the same name in 'source' (see QAction::ActiveShortcut)
    static void syncActiveShortcuts(KActionCollection *dest, const KActionCollection *source);

private:
    ViewManager *_viewManager;
    BookmarkHandler *_bookmarkHandler;
    KToggleAction *_toggleMenuBarAction;
    KActionMenu *_newTabMenuAction;

    QPointer<SessionController> _pluggedController;

    bool _menuBarInitialVisibility;
    bool _menuBarInitialVisibilityApplied;
};
}

#endif // MAINWINDOW_H
