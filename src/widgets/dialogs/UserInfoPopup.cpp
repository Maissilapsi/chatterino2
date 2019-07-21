#include "UserInfoPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/NetworkRequest.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/PartialTwitchUser.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "util/PostToThread.hpp"
#include "widgets/Label.hpp"
#include "widgets/dialogs/LogsPopup.hpp"
#include "widgets/helper/EffectLabel.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/helper/ChannelView.hpp"


#include <QCheckBox>
#include <QDesktopServices>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#define TEXT_FOLLOWERS "Followers: "
#define TEXT_VIEWS "Views: "
#define TEXT_CREATED "Created: "

namespace chatterino {

UserInfoPopup::UserInfoPopup()
    : BaseWindow(nullptr, BaseWindow::Flags(BaseWindow::Frameless |
                                            BaseWindow::FramelessDraggable))
    , hack_(new bool)
{
    this->setStayInScreenRect(true);

#ifdef Q_OS_LINUX
    this->setWindowFlag(Qt::Popup);
#endif

    auto app = getApp();

    auto layout =
        LayoutCreator<UserInfoPopup>(this).setLayoutType<QVBoxLayout>();

    // first line
    auto head = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        // avatar
        auto avatar =
            head.emplace<Button>(nullptr).assign(&this->ui_.avatarButton);
        avatar->setScaleIndependantSize(100, 100);
        QObject::connect(avatar.getElement(), &Button::leftClicked, [this] {
            QDesktopServices::openUrl(
                QUrl("https://twitch.tv/" + this->userName_.toLower()));
        });

        // items on the right
        auto vbox = head.emplace<QVBoxLayout>();
        {
            auto name = vbox.emplace<Label>().assign(&this->ui_.nameLabel);

            auto font = name->font();
            font.setBold(true);
            name->setFont(font);
            vbox.emplace<Label>(TEXT_VIEWS).assign(&this->ui_.viewCountLabel);
            vbox.emplace<Label>(TEXT_FOLLOWERS)
                .assign(&this->ui_.followerCountLabel);
            vbox.emplace<Label>(TEXT_CREATED)
                .assign(&this->ui_.createdDateLabel);
        }
    }

    layout.emplace<Line>(false);

    // second line
    auto user = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        user->addStretch(1);

        user.emplace<QCheckBox>("Follow").assign(&this->ui_.follow);
        user.emplace<QCheckBox>("Ignore").assign(&this->ui_.ignore);
        user.emplace<QCheckBox>("Ignore highlights")
            .assign(&this->ui_.ignoreHighlights);
        auto viewLogs = user.emplace<EffectLabel2>(this);
        viewLogs->getLabel().setText("Online logs");
        auto usercard = user.emplace<EffectLabel2>(this);
        usercard->getLabel().setText("Usercard");

        auto mod = user.emplace<Button>(this);
        mod->setPixmap(app->resources->buttons.mod);
        mod->setScaleIndependantSize(30, 30);
        auto unmod = user.emplace<Button>(this);
        unmod->setPixmap(app->resources->buttons.unmod);
        unmod->setScaleIndependantSize(30, 30);

        user->addStretch(1);

        QObject::connect(viewLogs.getElement(), &Button::leftClicked, [this] {
            auto logs = new LogsPopup();
            logs->setChannel(this->channel_);
            logs->setTargetUserName(this->userName_);
            logs->getLogs();
            logs->setAttribute(Qt::WA_DeleteOnClose);
            logs->show();
        });

        QObject::connect(usercard.getElement(), &Button::leftClicked, [this] {
            QDesktopServices::openUrl("https://www.twitch.tv/popout/" +
                                      this->channel_->getName() +
                                      "/viewercard/" + this->userName_);
        });

        QObject::connect(mod.getElement(), &Button::leftClicked, [this] {
            this->channel_->sendMessage("/mod " + this->userName_);
        });
        QObject::connect(unmod.getElement(), &Button::leftClicked, [this] {
            this->channel_->sendMessage("/unmod " + this->userName_);
        });

        // userstate
        this->userStateChanged_.connect([this, mod, unmod]() mutable {
            TwitchChannel *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->channel_.get());

            bool visibilityMod = false;
            bool visibilityUnmod = false;

            if (twitchChannel)
            {
                qDebug() << this->userName_;

                bool isMyself =
                    QString::compare(
                        getApp()->accounts->twitch.getCurrent()->getUserName(),
                        this->userName_, Qt::CaseInsensitive) == 0;

                visibilityMod = twitchChannel->isBroadcaster() && !isMyself;
                visibilityUnmod =
                    visibilityMod || (twitchChannel->isMod() && isMyself);
            }
            mod->setVisible(visibilityMod);
            unmod->setVisible(visibilityUnmod);
        });
    }

    auto lineMod = layout.emplace<Line>(false);

    // third line
    auto moderation = layout.emplace<QHBoxLayout>().withoutMargin();
    {
        auto timeout = moderation.emplace<TimeoutWidget>();

        this->userStateChanged_.connect([this, lineMod, timeout]() mutable {
            TwitchChannel *twitchChannel =
                dynamic_cast<TwitchChannel *>(this->channel_.get());

            bool hasModRights =
                twitchChannel ? twitchChannel->hasModRights() : false;
            lineMod->setVisible(hasModRights);
            timeout->setVisible(hasModRights);
        });

        timeout->buttonClicked.connect([this](auto item) {
            TimeoutWidget::Action action;
            int arg;
            std::tie(action, arg) = item;

            switch (action)
            {
                case TimeoutWidget::Ban:
                {
                    if (this->channel_)
                    {
                        this->channel_->sendMessage("/ban " + this->userName_);
                    }
                }
                break;
                case TimeoutWidget::Unban:
                {
                    if (this->channel_)
                    {
                        this->channel_->sendMessage("/unban " +
                                                    this->userName_);
                    }
                }
                break;
                case TimeoutWidget::Timeout:
                {
                    if (this->channel_)
                    {
                        this->channel_->sendMessage("/timeout " +
                                                    this->userName_ + " " +
                                                    QString::number(arg));
                    }
                }
                break;
            }
        });
    }

    // fourth line (last messages)
    this->latestMessages_ = new ChannelView();
    this->latestMessages_->setScaleIndependantHeight(150);
    layout.append(this->latestMessages_);

    this->setStyleSheet("font-size: 11pt;");

    this->installEvents();
}

void UserInfoPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    this->setStyleSheet("background: #333");
}

void UserInfoPopup::installEvents()
{
    std::weak_ptr<bool> hack = this->hack_;

    // follow
    QObject::connect(
        this->ui_.follow, &QCheckBox::stateChanged, [this](int) mutable {
            auto currentUser = getApp()->accounts->twitch.getCurrent();

            QUrl requestUrl("https://api.twitch.tv/kraken/users/" +
                            currentUser->getUserId() + "/follows/channels/" +
                            this->userId_);

            const auto reenableFollowCheckbox = [this] {
                this->ui_.follow->setEnabled(true);  //
            };

            this->ui_.follow->setEnabled(false);
            if (this->ui_.follow->isChecked())
            {
                currentUser->followUser(this->userId_, reenableFollowCheckbox);
            }
            else
            {
                currentUser->unfollowUser(this->userId_,
                                          reenableFollowCheckbox);
            }
        });

    std::shared_ptr<bool> ignoreNext = std::make_shared<bool>(false);

    // ignore
    QObject::connect(
        this->ui_.ignore, &QCheckBox::stateChanged,
        [this, ignoreNext, hack](int) mutable {
            if (*ignoreNext)
            {
                *ignoreNext = false;
                return;
            }

            this->ui_.ignore->setEnabled(false);

            auto currentUser = getApp()->accounts->twitch.getCurrent();
            if (this->ui_.ignore->isChecked())
            {
                currentUser->ignoreByID(
                    this->userId_, this->userName_,
                    [=](auto result, const auto &message) mutable {
                        if (hack.lock())
                        {
                            if (result == IgnoreResult_Failed)
                            {
                                *ignoreNext = true;
                                this->ui_.ignore->setChecked(false);
                            }
                            this->ui_.ignore->setEnabled(true);
                        }
                    });
            }
            else
            {
                currentUser->unignoreByID(
                    this->userId_, this->userName_,
                    [=](auto result, const auto &message) mutable {
                        if (hack.lock())
                        {
                            if (result == UnignoreResult_Failed)
                            {
                                *ignoreNext = true;
                                this->ui_.ignore->setChecked(true);
                            }
                            this->ui_.ignore->setEnabled(true);
                        }
                    });
            }
        });

    // ignore highlights
    QObject::connect(
        this->ui_.ignoreHighlights, &QCheckBox::clicked,
        [this](bool checked) mutable {
            this->ui_.ignoreHighlights->setEnabled(false);

            if (checked)
            {
                getApp()->highlights->blacklistedUsers.insertItem(
                    HighlightBlacklistUser{this->userName_, false});
                this->ui_.ignoreHighlights->setEnabled(true);
            }
            else
            {
                const auto &vector =
                    getApp()->highlights->blacklistedUsers.getVector();

                for (int i = 0; i < vector.size(); i++)
                {
                    if (this->userName_ == vector[i].getPattern())
                    {
                        getApp()->highlights->blacklistedUsers.removeItem(i);
                        i--;
                    }
                }
                if (getApp()->highlights->blacklistContains(this->userName_))
                {
                    this->ui_.ignoreHighlights->setToolTip(
                        "Name matched by regex");
                }
                else
                {
                    this->ui_.ignoreHighlights->setEnabled(true);
                }
            }
        });
}

void UserInfoPopup::setData(const QString &name, const ChannelPtr &channel)
{
    this->userName_ = name;
    this->channel_ = channel;

    this->ui_.nameLabel->setText(name);

    this->updateUserData();

    this->userStateChanged_.invoke();

    this->fillLatestMessages();
}

void UserInfoPopup::updateUserData()
{
    std::weak_ptr<bool> hack = this->hack_;

    const auto onIdFetched = [this, hack](QString id) {
        auto currentUser = getApp()->accounts->twitch.getCurrent();

        this->userId_ = id;

        QString url("https://api.twitch.tv/kraken/channels/" + id);

        auto request = NetworkRequest::twitchRequest(url);
        request.setCaller(this);

        request.onSuccess([this](auto result) -> Outcome {
            auto obj = result.parseJson();
            this->ui_.followerCountLabel->setText(
                TEXT_FOLLOWERS +
                QString::number(obj.value("followers").toInt()));
            this->ui_.viewCountLabel->setText(
                TEXT_VIEWS + QString::number(obj.value("views").toInt()));
            this->ui_.createdDateLabel->setText(
                TEXT_CREATED +
                obj.value("created_at").toString().section("T", 0, 0));

            this->loadAvatar(QUrl(obj.value("logo").toString()));

            this->loadProfileBanner(
                QUrl(obj.value("profile_banner").toString()));

            return Success;
        });

        request.execute();

        // get follow state
        currentUser->checkFollow(id, [this, hack](auto result) {
            if (hack.lock())
            {
                if (result != FollowResult_Failed)
                {
                    this->ui_.follow->setEnabled(true);
                    this->ui_.follow->setChecked(result ==
                                                 FollowResult_Following);
                }
            }
        });

        // get ignore state
        bool isIgnoring = false;
        for (const auto &ignoredUser : currentUser->getIgnores())
        {
            if (id == ignoredUser.id)
            {
                isIgnoring = true;
                break;
            }
        }

        // get ignoreHighlights state
        bool isIgnoringHighlights = false;
        const auto &vector = getApp()->highlights->blacklistedUsers.getVector();
        for (int i = 0; i < vector.size(); i++)
        {
            if (this->userName_ == vector[i].getPattern())
            {
                isIgnoringHighlights = true;
                break;
            }
        }
        if (getApp()->highlights->blacklistContains(this->userName_) &&
            !isIgnoringHighlights)
        {
            this->ui_.ignoreHighlights->setToolTip("Name matched by regex");
        }
        else
        {
            this->ui_.ignoreHighlights->setEnabled(true);
        }
        this->ui_.ignore->setEnabled(true);
        this->ui_.ignore->setChecked(isIgnoring);
        this->ui_.ignoreHighlights->setChecked(isIgnoringHighlights);
    };

    PartialTwitchUser::byName(this->userName_).getId(onIdFetched, this);

    this->ui_.follow->setEnabled(false);
    this->ui_.ignore->setEnabled(false);
    this->ui_.ignoreHighlights->setEnabled(false);
}

void UserInfoPopup::loadAvatar(const QUrl &url)
{
    QNetworkRequest req(url);
    static auto manager = new QNetworkAccessManager();
    auto *reply = manager->get(req);

    QObject::connect(reply, &QNetworkReply::finished, this, [=] {
        if (reply->error() == QNetworkReply::NoError)
        {
            const auto data = reply->readAll();

            // might want to cache the avatar image
            QPixmap avatar;
            avatar.loadFromData(data);
            this->ui_.avatarButton->setPixmap(avatar);
        }
        else
        {
            this->ui_.avatarButton->setPixmap(QPixmap());
        }
    });
}

void UserInfoPopup::loadProfileBanner(const QUrl &url)
{
    QNetworkRequest req(url);
    static auto manager = new QNetworkAccessManager();
    auto *reply = manager->get(req);

    QObject::connect(reply, &QNetworkReply::finished, this, [=] {
        if (reply->error() == QNetworkReply::NoError)
        {
            const auto data = reply->readAll();

            // might want to cache the banner image
            QPixmap banner;
            banner.loadFromData(data);
            this->ui_.profileBanner =
                banner.scaled(600, 200, Qt::KeepAspectRatio);
        }
        else
        {
            QPixmap emptyBanner;
            this->ui_.profileBanner = emptyBanner;
        }
    });
}

//
// TimeoutWidget
//
UserInfoPopup::TimeoutWidget::TimeoutWidget()
    : BaseWidget(nullptr)
{
    auto layout = LayoutCreator<TimeoutWidget>(this)
                      .setLayoutType<QHBoxLayout>()
                      .withoutMargin();

    QColor color1(255, 255, 255, 80);
    QColor color2(255, 255, 255, 0);

    int buttonWidth = 24;
    int buttonWidth2 = 40;
    int buttonHeight = 32;

    layout->setSpacing(16);

    auto addButton = [&](Action action, const QString &text,
                         const QPixmap &pixmap) {
        auto vbox = layout.emplace<QVBoxLayout>().withoutMargin();
        {
            auto title = vbox.emplace<QHBoxLayout>().withoutMargin();
            title->addStretch(1);
            auto label = title.emplace<Label>(text);
            label->setHasOffset(false);
            label->setStyleSheet("color: #BBB");
            title->addStretch(1);

            auto hbox = vbox.emplace<QHBoxLayout>().withoutMargin();
            hbox->setSpacing(0);
            {
                auto button = hbox.emplace<Button>(nullptr);
                button->setPixmap(pixmap);
                button->setScaleIndependantSize(buttonHeight, buttonHeight);
                button->setBorderColor(QColor(255, 255, 255, 127));

                QObject::connect(
                    button.getElement(), &Button::leftClicked, [this, action] {
                        this->buttonClicked.invoke(std::make_pair(action, -1));
                    });
            }
        }
    };

    auto addTimeouts = [&](const QString &title_,
                           const std::vector<std::pair<QString, int>> &items) {
        auto vbox = layout.emplace<QVBoxLayout>().withoutMargin();
        {
            auto title = vbox.emplace<QHBoxLayout>().withoutMargin();
            title->addStretch(1);
            auto label = title.emplace<Label>(title_);
            label->setStyleSheet("color: #BBB");
            label->setHasOffset(false);
            title->addStretch(1);

            auto hbox = vbox.emplace<QHBoxLayout>().withoutMargin();
            hbox->setSpacing(0);

            for (const auto &item : items)
            {
                auto a = hbox.emplace<EffectLabel2>();
                a->getLabel().setText(std::get<0>(item));

                a->setScaleIndependantSize(buttonWidth2, buttonHeight);
                a->setBorderColor(color1);

                QObject::connect(a.getElement(), &EffectLabel2::leftClicked,
                                 [this, timeout = std::get<1>(item)] {
                                     this->buttonClicked.invoke(std::make_pair(
                                         Action::Timeout, timeout));
                                 });
            }
        }
    };

    addButton(Unban, "unban", getApp()->resources->buttons.unban);

    addTimeouts("Timeouts",
                {{getSettings()->timeoutDurationPerUnit1.getValue() +
                      getSettings()->timeoutDurationUnit1.getValue(),
                  getSettings()->timeoutDurationInSec1.getValue()},

                 {getSettings()->timeoutDurationPerUnit2.getValue() +
                      getSettings()->timeoutDurationUnit2.getValue(),
                  getSettings()->timeoutDurationInSec2.getValue()},

                 {getSettings()->timeoutDurationPerUnit3.getValue() +
                      getSettings()->timeoutDurationUnit3.getValue(),
                  getSettings()->timeoutDurationInSec3.getValue()},

                 {getSettings()->timeoutDurationPerUnit4.getValue() +
                      getSettings()->timeoutDurationUnit4.getValue(),
                  getSettings()->timeoutDurationInSec4.getValue()},

                 {getSettings()->timeoutDurationPerUnit5.getValue() +
                      getSettings()->timeoutDurationUnit5.getValue(),
                  getSettings()->timeoutDurationInSec5.getValue()},

                 {getSettings()->timeoutDurationPerUnit6.getValue() +
                      getSettings()->timeoutDurationUnit6.getValue(),
                  getSettings()->timeoutDurationInSec6.getValue()},

                 {getSettings()->timeoutDurationPerUnit7.getValue() +
                      getSettings()->timeoutDurationUnit7.getValue(),
                  getSettings()->timeoutDurationInSec7.getValue()},

                 {getSettings()->timeoutDurationPerUnit8.getValue() +
                      getSettings()->timeoutDurationUnit8.getValue(),
                  getSettings()->timeoutDurationInSec8.getValue()}});

    addButton(Ban, "ban", getApp()->resources->buttons.ban);
}

void UserInfoPopup::TimeoutWidget::paintEvent(QPaintEvent *)
{
    //    QPainter painter(this);

    //    painter.setPen(QColor(255, 255, 255, 63));

    //    painter.drawLine(0, this->height() / 2, this->width(), this->height()
    //    / 2);
}

void UserInfoPopup::fillLatestMessages(){
    LimitedQueueSnapshot<MessagePtr> snapshot = this->channel_->getMessageSnapshot();
    ChannelPtr channelPtr(new Channel("search", Channel::Type::None));
    for (size_t i = 0; i < snapshot.size(); i++)
    {
        MessagePtr message = snapshot[i];
        if ( message->loginName.compare(this->userName_, Qt::CaseInsensitive) == 0
             && !message->flags.has(MessageFlag::Whisper) )
        {
            channelPtr->addMessage(message);
        }
    }

    this->latestMessages_->setChannel(channelPtr);
}

}  // namespace chatterino
