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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QString>
#include <QStringList>

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

    /// An account whose salt and hash were made earlier (the users file).
    void add_hashed(const QString& user, const QByteArray& salt,
                    const QByteArray& hash, Role role) {
        users_.insert(user, Credential{salt, hash, role});
    }

    [[nodiscard]] const QHash<QString, Credential>& all() const noexcept {
        return users_;
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

// ── THE USERS FILE: how a Release build gets its accounts ───────────────────
//
// A Release build has no default account, so the accounts live in a file the
// server's owner writes once: `altair_desktop --add-user NAME admin|staff`
// asks for the password on the terminal (never on the command line) and
// stores only the salt and the stretched hash.
//
// Where: $ALTAIR_USERS_FILE, else $XDG_CONFIG_HOME/altair/users, else
// ~/.config/altair/users -- next to the broker credential vault, same rules:
// the directory 0700, the file 0600. A file that group or others can read or
// write is REFUSED, not loaded: a hash anyone can replace is not a gate.
//
// One account per line:  name role salt-hex hash-hex   ('#' starts a comment)

[[nodiscard]] inline QString users_file_path() {
    const QString explicit_path = qEnvironmentVariable("ALTAIR_USERS_FILE");
    if (!explicit_path.isEmpty()) return explicit_path;
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (xdg.startsWith(QLatin1Char('/'))) return xdg + QStringLiteral("/altair/users");
    return QDir::homePath() + QStringLiteral("/.config/altair/users");
}

[[nodiscard]] inline Role role_from_name(const QString& name) {
    if (name == QStringLiteral("admin")) return Role::Admin;
    if (name == QStringLiteral("staff")) return Role::Staff;
    return Role::None;
}

/// Loads the users file into `store`. A missing file is not an error (the
/// login screen says nothing is provisioned). Returns false with `error` set
/// when the file is unsafe or unreadable; lines that do not parse are skipped
/// and counted in `error`.
inline bool load_users_file(UserStore& store, const QString& path, QString* error) {
    QFile f(path);
    if (!f.exists()) return true;
    const auto open_to_others = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                              | QFileDevice::ReadOther | QFileDevice::WriteOther;
    if (f.permissions() & open_to_others) {
        if (error) *error = QStringLiteral("%1 is readable or writable by others; "
                                           "not loaded (chmod 600 it)").arg(path);
        return false;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    int bad = 0;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const Role role = parts.size() == 4 ? role_from_name(parts[1]) : Role::None;
        const QByteArray salt = parts.size() == 4 ? QByteArray::fromHex(parts[2].toLatin1()) : QByteArray();
        const QByteArray hash = parts.size() == 4 ? QByteArray::fromHex(parts[3].toLatin1()) : QByteArray();
        if (role == Role::None || salt.size() != 16 || hash.size() != 32) {
            ++bad;
            continue;
        }
        store.add_hashed(parts[0], salt, hash, role);
    }
    if (bad > 0 && error) *error = QStringLiteral("%1: %2 line(s) not understood, skipped").arg(path).arg(bad);
    return true;
}

/// Adds or replaces one account in the users file (0600, directory 0700),
/// keeping the others. Returns false with `error` set on failure.
inline bool save_user(const QString& path, const QString& user,
                      const QString& password, Role role, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (user.isEmpty() || user.contains(QLatin1Char(' ')) || user.startsWith(QLatin1Char('#')))
        return fail(QStringLiteral("a user name has no spaces and does not start with #"));
    if (role == Role::None) return fail(QStringLiteral("the role is admin or staff"));
    if (password.isEmpty()) return fail(QStringLiteral("the password is empty"));

    UserStore store;
    QString load_error;
    if (!load_users_file(store, path, &load_error) ) return fail(load_error);
    store.add(user, password, role);

    const QFileInfo info(path);
    QDir dir = info.absoluteDir();
    if (!dir.exists()) {
        if (!QDir().mkpath(dir.absolutePath()))
            return fail(QStringLiteral("cannot create %1").arg(dir.absolutePath()));
        QFile::setPermissions(dir.absolutePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, out.errorString()));
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write("# ALTAIR accounts: name role salt hash (altair_desktop --add-user NAME admin|staff)\n");
    QStringList names = store.all().keys();
    names.sort();
    for (const QString& name : names) {
        const Credential& c = store.all()[name];
        out.write(QStringLiteral("%1 %2 %3 %4\n")
                      .arg(name, role_name(c.role), QString::fromLatin1(c.salt.toHex()),
                           QString::fromLatin1(c.hash.toHex()))
                      .toUtf8());
    }
    if (!out.commit()) return fail(QStringLiteral("cannot write %1: %2").arg(path, out.errorString()));
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace altair::ui
