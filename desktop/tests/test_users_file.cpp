// The users file: a Release build's only source of accounts (desktop/auth.hpp).
#include "../auth.hpp"

#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* description) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", description);
    if (!ok) ++failures;
}
} // namespace

int main() {
    using namespace altair::ui;
    QTemporaryDir tmp;
    const QString path = tmp.path() + QStringLiteral("/cfg/altair/users");
    QString error;

    UserStore none;
    check(load_users_file(none, path, &error) && none.empty(), "a missing file loads nothing and is not an error");

    check(save_user(path, QStringLiteral("smit"), QStringLiteral("s3cret"), Role::Admin, &error), "save an admin");
    check(save_user(path, QStringLiteral("desk"), QStringLiteral("view"), Role::Staff, &error), "save a staff account");
    check(QFile::permissions(path) == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser),
          "the file is 0600");

    UserStore store;
    error.clear();
    check(load_users_file(store, path, &error) && error.isEmpty() && store.count() == 2, "both accounts load back");
    check(store.verify(QStringLiteral("smit"), QStringLiteral("s3cret")) == Role::Admin, "the admin signs in");
    check(store.verify(QStringLiteral("desk"), QStringLiteral("view")) == Role::Staff, "the staff account signs in");
    check(store.verify(QStringLiteral("smit"), QStringLiteral("wrong")) == Role::None, "a wrong password is refused");
    QFile raw(path);
    check(raw.open(QIODevice::ReadOnly) && !raw.readAll().contains("s3cret"), "the password is not stored");

    check(save_user(path, QStringLiteral("smit"), QStringLiteral("new"), Role::Admin, &error), "replace a password");
    UserStore again;
    check(load_users_file(again, path, &error) && again.count() == 2
              && again.verify(QStringLiteral("smit"), QStringLiteral("new")) == Role::Admin
              && again.verify(QStringLiteral("smit"), QStringLiteral("s3cret")) == Role::None,
          "the new password replaces the old one, other accounts kept");

    check(!save_user(path, QStringLiteral("a b"), QStringLiteral("x"), Role::Admin, &error), "a name with a space is refused");
    check(!save_user(path, QStringLiteral("x"), QString(), Role::Admin, &error), "an empty password is refused");

    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ReadOther);
    UserStore open_file;
    check(!load_users_file(open_file, path, &error) && open_file.empty(), "a world-readable file is refused");

    std::printf("%s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
