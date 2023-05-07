// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <QApplication>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>
#include "common/fs/path_util.h"
#include "common/string_util.h"
#include "core/constants.h"
#include "yuzu/applets/qt_profile_select.h"
#include "yuzu/main.h"
#include "yuzu/util/controller_navigation.h"

namespace {
QString FormatUserEntryText(const QString& username, Common::UUID uuid) {
    return QtProfileSelectionDialog::tr(
               "%1\n%2", "%1 is the profile username, %2 is the formatted UUID (e.g. "
                         "00112233-4455-6677-8899-AABBCCDDEEFF))")
        .arg(username, QString::fromStdString(uuid.FormattedString()));
}

QString GetImagePath(Common::UUID uuid) {
    const auto path =
        Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) /
        fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString());
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QPixmap GetIcon(Common::UUID uuid) {
    QPixmap icon{GetImagePath(uuid)};

    if (!icon) {
        icon.fill(Qt::black);
        icon.loadFromData(Core::Constants::ACCOUNT_BACKUP_JPEG.data(),
                          static_cast<u32>(Core::Constants::ACCOUNT_BACKUP_JPEG.size()));
    }

    return icon.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
} // Anonymous namespace

QtProfileSelectionDialog::QtProfileSelectionDialog(
    Core::HID::HIDCore& hid_core, QWidget* parent,
    const Core::Frontend::ProfileSelectParameters& parameters)
    : QDialog(parent), profile_manager(std::make_unique<Service::Account::ProfileManager>()) {
    outer_layout = new QVBoxLayout;

    instruction_label = new QLabel();

    scroll_area = new QScrollArea;

    buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QtProfileSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QtProfileSelectionDialog::reject);

    outer_layout->addWidget(instruction_label);
    outer_layout->addWidget(scroll_area);
    outer_layout->addWidget(buttons);

    layout = new QVBoxLayout;
    tree_view = new QTreeView;
    item_model = new QStandardItemModel(tree_view);
    tree_view->setModel(item_model);
    controller_navigation = new ControllerNavigation(hid_core, this);

    tree_view->setAlternatingRowColors(true);
    tree_view->setSelectionMode(QHeaderView::SingleSelection);
    tree_view->setSelectionBehavior(QHeaderView::SelectRows);
    tree_view->setVerticalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setHorizontalScrollMode(QHeaderView::ScrollPerPixel);
    tree_view->setSortingEnabled(true);
    tree_view->setEditTriggers(QHeaderView::NoEditTriggers);
    tree_view->setUniformRowHeights(true);
    tree_view->setIconSize({64, 64});
    tree_view->setContextMenuPolicy(Qt::NoContextMenu);

    item_model->insertColumns(0, 1);
    item_model->setHeaderData(0, Qt::Horizontal, tr("Users"));

    // We must register all custom types with the Qt Automoc system so that we are able to use it
    // with signals/slots. In this case, QList falls under the umbrella of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tree_view);

    scroll_area->setLayout(layout);

    connect(tree_view, &QTreeView::clicked, this, &QtProfileSelectionDialog::SelectUser);
    connect(tree_view, &QTreeView::doubleClicked, this, &QtProfileSelectionDialog::accept);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent,
            [this](Qt::Key key) {
                if (!this->isActiveWindow()) {
                    return;
                }
                QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::postEvent(tree_view, event);
                SelectUser(tree_view->currentIndex());
            });

    const auto& profiles = profile_manager->GetAllUsers();
    for (const auto& user : profiles) {
        Service::Account::ProfileBase profile{};
        if (!profile_manager->GetProfileBase(user, profile))
            continue;

        const auto username = Common::StringFromFixedZeroTerminatedBuffer(
            reinterpret_cast<const char*>(profile.username.data()), profile.username.size());

        list_items.push_back(QList<QStandardItem*>{new QStandardItem{
            GetIcon(user), FormatUserEntryText(QString::fromStdString(username), user)}});
    }

    for (const auto& item : list_items)
        item_model->appendRow(item);

    setLayout(outer_layout);
    SetWindowTitle(parameters);
    SetDialogPurpose(parameters);
    resize(550, 400);
}

QtProfileSelectionDialog::~QtProfileSelectionDialog() {
    controller_navigation->UnloadController();
};

int QtProfileSelectionDialog::exec() {
    // Skip profile selection when there's only one.
    if (profile_manager->GetUserCount() == 1) {
        user_index = 0;
        return QDialog::Accepted;
    }
    return QDialog::exec();
}

void QtProfileSelectionDialog::accept() {
    QDialog::accept();
}

void QtProfileSelectionDialog::reject() {
    user_index = 0;
    QDialog::reject();
}

int QtProfileSelectionDialog::GetIndex() const {
    return user_index;
}

void QtProfileSelectionDialog::SelectUser(const QModelIndex& index) {
    user_index = index.row();
}

void QtProfileSelectionDialog::SetWindowTitle(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    using Service::AM::Applets::UiMode;
    switch (parameters.mode) {
    case UiMode::UserCreator:
    case UiMode::UserCreatorForStarter:
        setWindowTitle(tr("Profile Creator"));
        return;
    case UiMode::EnsureNetworkServiceAccountAvailable:
        setWindowTitle(tr("Profile Selector"));
        return;
    case UiMode::UserIconEditor:
        setWindowTitle(tr("Profile Icon Editor"));
        return;
    case UiMode::UserNicknameEditor:
        setWindowTitle(tr("Profile Nickname Editor"));
        return;
    case UiMode::NintendoAccountAuthorizationRequestContext:
    case UiMode::IntroduceExternalNetworkServiceAccount:
    case UiMode::IntroduceExternalNetworkServiceAccountForRegistration:
    case UiMode::NintendoAccountNnidLinker:
    case UiMode::LicenseRequirementsForNetworkService:
    case UiMode::LicenseRequirementsForNetworkServiceWithUserContextImpl:
    case UiMode::UserCreatorForImmediateNaLoginTest:
    case UiMode::UserQualificationPromoter:
    case UiMode::UserSelector:
    default:
        setWindowTitle(tr("Profile Selector"));
    }
}

void QtProfileSelectionDialog::SetDialogPurpose(
    const Core::Frontend::ProfileSelectParameters& parameters) {
    using Service::AM::Applets::UserSelectionPurpose;

    switch (parameters.purpose) {
    case UserSelectionPurpose::GameCardRegistration:
        instruction_label->setText(tr("Who will receive the points?"));
        return;
    case UserSelectionPurpose::EShopLaunch:
        instruction_label->setText(tr("Who is using Nintendo eShop?"));
        return;
    case UserSelectionPurpose::EShopItemShow:
        instruction_label->setText(tr("Who is making this purchase?"));
        return;
    case UserSelectionPurpose::PicturePost:
        instruction_label->setText(tr("Who is posting?"));
        return;
    case UserSelectionPurpose::NintendoAccountLinkage:
        instruction_label->setText(tr("Select a user to link to a Nintendo Account."));
        return;
    case UserSelectionPurpose::SettingsUpdate:
        instruction_label->setText(tr("Change settings for which user?"));
        return;
    case UserSelectionPurpose::SaveDataDeletion:
        instruction_label->setText(tr("Format data for which user?"));
        return;
    case UserSelectionPurpose::UserMigration:
        instruction_label->setText(tr("Which user will be transferred to another console?"));
        return;
    case UserSelectionPurpose::SaveDataTransfer:
        instruction_label->setText(tr("Send save data for which user?"));
        return;
    case UserSelectionPurpose::General:
    default:
        instruction_label->setText(tr("Select a user:"));
        return;
    }
}

QtProfileSelector::QtProfileSelector(GMainWindow& parent) {
    connect(this, &QtProfileSelector::MainWindowSelectProfile, &parent,
            &GMainWindow::ProfileSelectorSelectProfile, Qt::QueuedConnection);
    connect(this, &QtProfileSelector::MainWindowRequestExit, &parent,
            &GMainWindow::ProfileSelectorRequestExit, Qt::QueuedConnection);
    connect(&parent, &GMainWindow::ProfileSelectorFinishedSelection, this,
            &QtProfileSelector::MainWindowFinishedSelection, Qt::DirectConnection);
}

QtProfileSelector::~QtProfileSelector() = default;

void QtProfileSelector::Close() const {
    callback = {};
    emit MainWindowRequestExit();
}

void QtProfileSelector::SelectProfile(
    SelectProfileCallback callback_,
    const Core::Frontend::ProfileSelectParameters& parameters) const {
    callback = std::move(callback_);
    emit MainWindowSelectProfile(parameters);
}

void QtProfileSelector::MainWindowFinishedSelection(std::optional<Common::UUID> uuid) {
    if (callback) {
        callback(uuid);
    }
}
