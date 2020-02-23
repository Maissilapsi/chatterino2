#pragma once

#include "common/SignalVector.hpp"
#include "common/Singleton.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"

namespace chatterino {

class HighlightController final : public Singleton
{
public:
    virtual void initialize(Settings &settings, Paths &paths) override;

    SignalVector<HighlightPhrase> phrases;
    SignalVector<HighlightBlacklistUser> blacklistedUsers;
    SignalVector<HighlightPhrase> highlightedUsers;

    bool isHighlightedUser(const QString &username);
    bool blacklistContains(const QString &username);

private:
    bool initialized_ = false;
};

}  // namespace chatterino
