// desktop/format.hpp -- money on screen.
//
// P11Q-06 moved this out of `tick_model.hpp`. It is a formatting utility, not
// model code, and leaving it there meant every caller that wanted to print a
// price had to drag in a `Q_OBJECT` class and a moc pass with it.
//
// MONEY IS int64 PAISE UNTIL THE MOMENT IT IS DRAWN, and this is that moment.
// The only division by 100 in the UI is below, and it is integer division:
// the rupee part and the paise part are separated with `/` and `%`, never by
// dividing a double. Rule 3 does not stop at the socket.

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace altair::ui {

/// Paise to a rupee string with Indian digit grouping, exactly.
///
/// Integer arithmetic throughout: the rupee part and the paise part are
/// separated with `/` and `%`, never by dividing a double by 100. Ported from
/// the web client's `formatPaise`, which was written for the same reason.
[[nodiscard]] inline QString format_paise(std::int64_t paise,
                                          bool explicit_sign = false) {
    const bool negative = paise < 0;
    const std::int64_t abs_paise = negative ? -paise : paise;
    const std::int64_t rupees = abs_paise / 100;
    const std::int64_t fraction = abs_paise % 100;

    QString digits = QString::number(rupees);
    // Indian grouping: last three digits, then pairs. 12345678 reads as
    // 1,23,45,678 -- one crore twenty-three lakh.
    if (digits.size() > 3) {
        QString head = digits.left(digits.size() - 3);
        const QString tail = digits.right(3);
        QStringList parts;
        while (head.size() > 2) {
            parts.prepend(head.right(2));
            head.chop(2);
        }
        if (!head.isEmpty()) {
            parts.prepend(head);
        }
        digits = parts.join(QLatin1Char(',')) + QLatin1Char(',') + tail;
    }

    const QString sign = negative ? QStringLiteral("-")
                       : explicit_sign ? QStringLiteral("+")
                                       : QString();
    return QStringLiteral("%1%2.%3")
        .arg(sign, digits, QString::number(fraction).rightJustified(2, u'0'));
}

} // namespace altair::ui
