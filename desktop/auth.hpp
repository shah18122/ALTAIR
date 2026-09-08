// desktop/auth.hpp -- who is at the keyboard, and what they may do.
//
// P11Q-05a.
//
// DEFAULT CREDENTIALS MUST NOT SURVIVE THE BUILD THAT SHIPS.
//
// Smit asked for admin/admin and staff/staff "when opening this UI but in
// future not while building it". That second clause is the whole card, because
// default credentials are the most reliably exploited weakness in deployed
// software and they are never removed on purpose -- they are removed when
// somebody remembers.
//
// So they are not a constant that a later commit is expected to delete. They
// exist only when `ALTAIR_DEV_CREDENTIALS` is defined, which the `prod` preset
// does not define, and `dev_credentials_active()` is compiled out with them.
// A production build has no default account to disable: `seed_default_users`
// is an empty function and the login screen has nothing to log in as until a
// real user is provisioned.
//
// While they ARE active the window says so, in orange, permanently. A warning
// that can be dismissed is a warning that will be.
//
// THE PASSWORD IS NOT A SECRET, AND THE CODE SAYS SO.
//
// The same argument as P11-02b's PIN. This is a stretched hash rather than a
// plain one, and stretching buys time against someone who already has the
// machine -- it does not make "admin" a password. What this gate is actually
// for is stopping the person who sits down at an unattended desk, and it is
// good at that.
//
// A ROLE IS A SET OF CAPABILITIES, NOT A LABEL.
//
// `Role` on its own would be a string somebody compares in one place and
// forgets in another. `may()` is the only thing that answers, every call site
// asks it, and the capability list is short enough to read: Staff can see
// everything and change nothing that reaches the engine. Only Admin can even
// REQUEST the kill switch -- which still goes through P11-14's confirmation,
// because a role is not a substitute for saying what will happen.

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QRandomGenerator>
#include <QString>

#include <cstdint>

namespace altair::ui {

enum class Role : std::uint8_t {
    /// Phantom default. A zeroed session is nobody, not a guest.
    None = 0,
    /// Reads everything, changes nothing that reaches the engine.
    Staff,
    /// Everything Staff can do, plus requesting the kill switch and changing
    /// the feed source.
    Admin
};

[[nodiscard]] inline QString role_name(Role r) {
    switch (r) {
    case Role::Admin: return QStringLiteral("admin");
    case Role::Staff: return QStringLiteral("staff");
    case Role::None:
    default:          return QStringLiteral("signed out");
    }
}

/// What a signed-in person may do. Every capability that touches the engine is
/// here; anything not listed is readable by both roles.
enum class Capability : std::uint8_t {
    Unspecified = 0,
    /// Ask for the kill switch. Still requires P11-14's confirmation.
    RequestKillSwitch,
    /// Change which feed the UI is attached to.
    ChangeFeedSource,
    /// Add or remove instruments from the watchlist.
    EditWatchlist,
    /// Start a model training run.
    TrainModel,
    /// P25-04. Put an ORDER INTENT on the queue oms/ drains.
    ///
    /// Not "place an order" -- the UI cannot place one and this capability
    /// does not give it the ability. It gates writing a REQUEST that oms/ will
    /// validate against the spec store and may refuse. Admin only: Staff can
    /// see every position and every number in this window and still not ask
    /// for a trade, which is the distinction the two roles exist to draw.
    RequestOrder
};

[[nodiscard]] inline bool may(Role r, Capability c) noexcept {
    if (r == Role::None || c == Capability::Unspecified) {
        return false;
    }
    if (r == Role::Admin) {
        return true;
    }
    // Staff. Reads everything; the list below is what it may also DO.
    switch (c) {
    case Capability::EditWatchlist:
        return true;    // a watchlist is a view, not a position
    case Capability::RequestKillSwitch:
    case Capability::ChangeFeedSource:
    case Capability::TrainModel:
    case Capability::RequestOrder:
        return false;
    case Capability::Unspecified:
    default:
        return false;
    }
}

/// Iterations for the stretched hash. Not a tuning knob to lower "if login
/// feels slow": the cost per attempt is the only protection this offers, and
/// it is already modest.
inline constexpr int kHashRounds = 200'000;

struct Credential {
    QByteArray salt;
    QByteArray hash;
    Role role = Role::None;
};

/// Salt + repeated SHA-256. Deliberately NOT a bare hash.
///
/// A real KDF (PBKDF2, scrypt, argon2) would be better and lives in
/// QtNetwork's QPasswordDigestor, which this target does not link. The
/// stretching below is the honest middle: far better than one round, worse
/// than a purpose-built KDF, and the comment says which so nobody reads this
/// as stronger than it is.
[[nodiscard]] inline QByteArray stretch(const QString& password,
                                        const QByteArray& salt) {
    QByteArray acc = salt + password.toUtf8();
    for (int i = 0; i < kHashRounds; ++i) {
        acc = QCryptographicHash::hash(acc, QCryptographicHash::Sha256);
    }
    return acc;
}

/// Constant-time compare. An early return leaks the matching prefix.
[[nodiscard]] inline bool equal_constant_time(const QByteArray& a,
                                              const QByteArray& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(
            diff | (static_cast<unsigned char>(a[i])
                    ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
}

class UserStore {
public:
    void add(const QString& user, const QString& password, Role role) {
        Credential c;
        c.salt.resize(16);
        QRandomGenerator::system()->generate(
            reinterpret_cast<quint32*>(c.salt.data()),
            reinterpret_cast<quint32*>(c.salt.data() + c.salt.size()));
        c.hash = stretch(password, c.salt);
        c.role = role;
        users_.insert(user, c);
    }

    /// Returns `Role::None` for both "no such user" and "wrong password" --
    /// deliberately the same answer, so the screen cannot be used to enumerate
    /// which accounts exist.
    [[nodiscard]] Role verify(const QString& user,
                              const QString& password) const {
        const auto it = users_.constFind(user);
        if (it == users_.constEnd()) {
            // Still do the work, so a missing user is not measurably faster
            // than a wrong password.
            QByteArray dummy(16, '\0');
            (void)stretch(password, dummy);
            return Role::None;
        }
        return equal_constant_time(stretch(password, it->salt), it->hash)
                 ? it->role
                 : Role::None;
    }

    [[nodiscard]] bool empty() const noexcept { return users_.isEmpty(); }
    [[nodiscard]] int count() const noexcept {
        return static_cast<int>(users_.size());
    }

private:
    QHash<QString, Credential> users_;
};

/// Are the development accounts compiled in?
///
/// A runtime question with a compile-time answer, so the banner cannot say one
/// thing while the build does another.
[[nodiscard]] inline constexpr bool dev_credentials_active() noexcept {
#ifdef ALTAIR_DEV_CREDENTIALS
    return true;
#else
    return false;
#endif
}

/// Install admin/admin and staff/staff -- ONLY in a build that defined
/// `ALTAIR_DEV_CREDENTIALS`.
///
/// In every other build this is an empty function and the store stays empty,
/// which the login screen reports as "no accounts provisioned" rather than
/// letting anyone in. There is no default account to forget to remove.
inline void seed_default_users(UserStore& store) {
#ifdef ALTAIR_DEV_CREDENTIALS
    store.add(QStringLiteral("admin"), QStringLiteral("admin"), Role::Admin);
    store.add(QStringLiteral("staff"), QStringLiteral("staff"), Role::Staff);
#else
    (void)store;
#endif
}

} // namespace altair::ui
