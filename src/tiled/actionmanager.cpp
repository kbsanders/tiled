/*
 * actionmanager.cpp
 * Copyright 2016, Thorbjørn Lindeijer <bjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "actionmanager.h"

#include "preferences.h"

#include <QMenu>

namespace Tiled {

ActionManager::ActionManager(QObject *parent)
    : QObject(parent)
    , mMenuSeparatorsParent(new QObject)
{
    readCustomShortcuts();

    mIdToMenu.insert(MenuIds::layerViewLayers, nullptr);
    mIdToMenu.insert(MenuIds::mapViewObjects, nullptr);
    mIdToMenu.insert(MenuIds::projectViewFiles, nullptr);
    mIdToMenu.insert(MenuIds::propertiesViewProperties, nullptr);
    mIdToMenu.insert(MenuIds::tilesetViewTiles, nullptr);
}

ActionManager::~ActionManager() = default;

ActionManager *ActionManager::instance()
{
    static ActionManager instance;
    return &instance;
}

void ActionManager::registerAction(QAction *action, Id id)
{
    auto d = instance();

    Q_ASSERT_X(!d->mIdToActions.contains(id, action), "ActionManager::registerAction", "action already registered");
    d->mIdToActions.insert(id, action);
    d->mLastKnownShortcuts.insert(id, action->shortcut());

    connect(action, &QAction::changed, d, [=] {
        if (d->mApplyingToolTipWithShortcut)
            return;

        if (!d->mApplyingShortcut && d->mDefaultShortcuts.contains(id) && d->mLastKnownShortcuts.value(id) != action->shortcut()) {
            // Update remembered default shortcut
            d->mDefaultShortcuts.insert(id, action->shortcut());

            // Reset back to user-defined shortcut if set
            if (d->mCustomShortcuts.contains(id)) {
                d->applyShortcut(action, d->mCustomShortcuts.value(id));
                return;
            }
        }

        d->mLastKnownShortcuts.insert(id, action->shortcut());

        d->updateToolTipWithShortcut(action);

        emit d->actionChanged(id);
    });

    if (d->hasCustomShortcut(id)) {
        d->mDefaultShortcuts.insert(id, action->shortcut());
        d->applyShortcut(action, d->mCustomShortcuts.value(id));
    }

    d->updateToolTipWithShortcut(action);

    emit d->actionsChanged();
}

void ActionManager::unregisterAction(QAction *action, Id id)
{
    auto d = instance();

    Q_ASSERT_X(d->mIdToActions.contains(id, action), "ActionManager::unregisterAction", "unknown action");
    d->mIdToActions.remove(id, action);
    action->disconnect(d);
    d->mDefaultShortcuts.remove(id);
    d->mLastKnownShortcuts.remove(id);
    emit d->actionsChanged();
}

void ActionManager::registerMenu(QMenu *menu, Id id)
{
    auto d = instance();

    Q_ASSERT_X(!d->mIdToMenu.contains(id), "ActionManager::registerMenu", "duplicate id");
    d->mIdToMenu.insert(id, menu);

    if (menu)
        applyMenuExtensions(menu, id);
}

void ActionManager::unregisterMenu(Id id)
{
    auto d = instance();

    Q_ASSERT_X(d->mIdToMenu.contains(id), "ActionManager::unregisterMenu", "unknown id");
    d->mIdToMenu.remove(id);
}

void ActionManager::registerMenuExtension(Id id, MenuExtension extension)
{
    auto d = instance();
    d->mIdToMenuExtensions[id].append(extension);

    if (QMenu *menu = instance()->mIdToMenu.value(id))
        d->applyMenuExtension(menu, extension);
}

void ActionManager::applyMenuExtensions(QMenu *menu, Id id)
{
    auto d = instance();

    Q_ASSERT_X(d->mIdToMenu.contains(id), "ActionManager::applyMenuExtensions", "unknown id");
    const auto extensions = d->mIdToMenuExtensions.value(id);
    for (const auto &extension : extensions)
        d->applyMenuExtension(menu, extension);
}

void ActionManager::clearMenuExtensions()
{
    auto d = instance();
    d->mIdToMenuExtensions.clear();
    d->mMenuSeparatorsParent.reset(new QObject);
}

QAction *ActionManager::action(Id id)
{
    auto d = instance();

    auto action = d->mIdToActions.value(id);
    Q_ASSERT_X(action, "ActionManager::action", "unknown id");
    return action;
}

QAction *ActionManager::findAction(Id id)
{
    auto d = instance();

    return d->mIdToActions.value(id);
}

bool ActionManager::hasMenu(Id id)
{
    return instance()->mIdToMenu.contains(id);
}

QList<Id> ActionManager::actions()
{
    return instance()->mIdToActions.uniqueKeys();
}

QList<Id> ActionManager::menus()
{
    return instance()->mIdToMenu.keys();
}

void ActionManager::setCustomShortcut(Id id, const QKeySequence &keySequence)
{
    Q_ASSERT(!mResettingShortcut);

    const auto actions = mIdToActions.values(id);
    Q_ASSERT_X(!actions.isEmpty(), "ActionManager::setCustomShortcut", "unknown id");

    if (!hasCustomShortcut(id))
        mDefaultShortcuts.insert(id, actions.first()->shortcut());

    mCustomShortcuts.insert(id, keySequence);

    for (QAction *a : actions)
        applyShortcut(a, keySequence);

    Preferences::instance()->setValue(QLatin1String("CustomShortcuts/") + id.toString(),
                                      keySequence.toString());
}

bool ActionManager::hasCustomShortcut(Id id) const
{
    return mCustomShortcuts.contains(id);
}

void ActionManager::resetCustomShortcut(Id id)
{
    if (!hasCustomShortcut(id))
        return;

    const auto actions = mIdToActions.values(id);
    Q_ASSERT_X(!actions.isEmpty(), "ActionManager::resetCustomShortcut", "unknown id");

    mResettingShortcut = true;

    const QKeySequence defaultShortcut = mDefaultShortcuts.take(id);
    for (QAction *a : actions)
        applyShortcut(a, defaultShortcut);
    mCustomShortcuts.remove(id);

    mResettingShortcut = false;

    Preferences::instance()->remove(QLatin1String("CustomShortcuts/") + id.toString());
}

void ActionManager::resetAllCustomShortcuts()
{
    QHashIterator<Id, QKeySequence> iterator(mDefaultShortcuts);
    while (iterator.hasNext()) {
        iterator.next();
        const auto actions = mIdToActions.values(iterator.key());
        for (QAction *a : actions)
            applyShortcut(a, iterator.value());
    }
    mDefaultShortcuts.clear();
    mCustomShortcuts.clear();

    Preferences::instance()->remove(QLatin1String("CustomShortcuts"));
}

QKeySequence ActionManager::defaultShortcut(Id id)
{
    if (mDefaultShortcuts.contains(id))
        return mDefaultShortcuts.value(id);
    if (auto a = findAction(id))
        return a->shortcut();
    return QKeySequence();
}

/**
 * Sets the custom shortcuts.
 *
 * Shortcuts that are the same as the default ones will be reset.
 */
void ActionManager::setCustomShortcuts(const QHash<Id, QKeySequence> &shortcuts)
{
    QHashIterator<Id, QKeySequence> iterator(shortcuts);
    while (iterator.hasNext()) {
        iterator.next();

        const Id id = iterator.key();
        const QKeySequence &shortcut = iterator.value();

        if (auto a = findAction(id)) {
            if (mDefaultShortcuts.contains(id)
                    ? mDefaultShortcuts.value(id) == shortcut
                    : a->shortcut() == shortcut) {
                resetCustomShortcut(id);
            } else {
                setCustomShortcut(id, shortcut);
            }
        }
    }
}

void ActionManager::readCustomShortcuts()
{
    const auto prefs = Preferences::instance();
    prefs->beginGroup(QStringLiteral("CustomShortcuts"));

    const auto keys = prefs->childKeys();
    for (const auto &key : keys) {
        auto keySequence = QKeySequence::fromString(prefs->value(key).toString());
        mCustomShortcuts.insert(Id(key.toUtf8()), keySequence);
    }

    prefs->endGroup();
}

void ActionManager::applyShortcut(QAction *action, const QKeySequence &shortcut)
{
    mApplyingShortcut = true;
    action->setShortcut(shortcut);
    mApplyingShortcut = false;
}

void ActionManager::updateToolTipWithShortcut(QAction *action)
{
    mApplyingToolTipWithShortcut = true;

    QString toolTip = action->toolTip();

    // If shortcut present, unset shortut and retrieve stripped text
    if (toolTip.contains(QLatin1String(" <span "))) {
        action->setToolTip(QString());
        toolTip = action->toolTip();
    }

    if (!action->shortcut().isEmpty()) {
        toolTip.append(QStringLiteral(" <span style=\"color: gray;\">(%1)<span>")
                       .arg(action->shortcut().toString(QKeySequence::NativeText)));
    }

    action->setToolTip(toolTip);

    mApplyingToolTipWithShortcut = false;
}

void ActionManager::applyMenuExtension(QMenu *menu, const ActionManager::MenuExtension &extension)
{
    QAction *before = nullptr;

    for (const MenuItem &item : extension.items) {
        if (item.beforeAction)
            before = ActionManager::findAction(item.beforeAction);

        if (item.isSeparator)
            menu->insertSeparator(before)->setParent(mMenuSeparatorsParent.get());
        else
            menu->insertAction(before, ActionManager::action(item.action));
    }
}

} // namespace Tiled

#include "moc_actionmanager.cpp"
