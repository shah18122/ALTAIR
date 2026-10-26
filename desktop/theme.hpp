// desktop/theme.hpp -- one application-wide look, applied once in main().
//
// Before this, only the toolbar and a few panels styled themselves; every
// other control (tables, tabs, inputs, dialogs, scroll bars) fell back to the
// platform's light default, so each page was half dark terminal and half
// Windows 2000. This sets the Fusion style, a dark palette and one stylesheet
// built from a small set of web-style design tokens, so every standard
// control matches.
//
// Custom-painted widgets (chart, depth ladder, grid) keep their own painting.
// Colours that carry meaning (bid/ask, stale/fresh evidence) stay where they
// are set. A widget's own setStyleSheet() still wins over this one.
#pragma once

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QVariant>
#include <QWidget>

namespace altair::ui {

/// Design tokens. Neutral dark surfaces, one accent (Altair amber).
namespace theme_token {
inline constexpr const char* kCanvas     = "#0D1117";  // window background
inline constexpr const char* kSurface    = "#161B22";  // cards, headers, menus
inline constexpr const char* kSurfaceAlt = "#11161D";  // alternate rows
inline constexpr const char* kRaised     = "#21262D";  // buttons, hover
inline constexpr const char* kBorder     = "#30363D";
inline constexpr const char* kBorderHi   = "#484F58";
inline constexpr const char* kText       = "#E6EDF3";
inline constexpr const char* kTextMuted  = "#8B949E";
inline constexpr const char* kTextFaint  = "#6E7681";
inline constexpr const char* kAccent     = "#E3A34A";
inline constexpr const char* kAccentHi   = "#F0B765";
inline constexpr const char* kSelection  = "#493D2C";  // accent at 25% on surface
inline constexpr const char* kLink       = "#58A6FF";
// Meaning. Up/ok, down/error, caution, information.
inline constexpr const char* kUp         = "#3FB950";
inline constexpr const char* kDown       = "#F85149";
inline constexpr const char* kWarn       = "#D29922";
inline constexpr const char* kInfo       = "#58A6FF";
inline constexpr const char* kLive       = "#DA3633";  // real money
inline constexpr const char* kHalted     = "#6E1B1B";
} // namespace theme_token

[[nodiscard]] inline QString app_style_sheet() {
    return QStringLiteral(R"QSS(
QToolTip { background:#21262D; color:#E6EDF3; border:1px solid #484F58; border-radius:6px; padding:6px 8px; }
QMainWindow, QDialog { background:#0D1117; }

QMenuBar { background:#0D1117; color:#E6EDF3; border:0; border-bottom:1px solid #21262D; padding:2px 6px; }
QMenuBar::item { background:transparent; padding:6px 10px; border-radius:6px; }
QMenuBar::item:selected { background:#21262D; color:#F0B765; }
QMenu { background:#161B22; color:#E6EDF3; border:1px solid #30363D; border-radius:8px; padding:6px; }
QMenu::item { padding:7px 24px 7px 12px; border-radius:6px; }
QMenu::item:selected { background:#21262D; color:#F0B765; }
QMenu::item:disabled { color:#6E7681; }
QMenu::separator { height:1px; background:#30363D; margin:6px 8px; }

QToolBar { background:#0D1117; border:0; border-bottom:1px solid #21262D; padding:6px 10px; spacing:6px; }
QToolBar::separator { width:1px; background:#30363D; margin:4px 6px; }
QToolButton { color:#C9D1D9; background:transparent; border:1px solid transparent; border-radius:6px; padding:5px 10px; }
QToolButton:hover { background:#21262D; color:#E6EDF3; }
QToolButton:pressed, QToolButton:checked { background:#2D333B; color:#F0B765; }
QToolButton:focus { border-color:#E3A34A; }

QPushButton { color:#E6EDF3; background:#21262D; border:1px solid #363B42; border-radius:6px; padding:6px 14px; min-height:18px; }
QPushButton:hover { background:#292E36; border-color:#8B949E; }
QPushButton:pressed { background:#1C2128; }
QPushButton:focus { border-color:#E3A34A; }
QPushButton:default { background:#E3A34A; color:#0D1117; border-color:#E3A34A; font-weight:600; }
QPushButton:default:hover { background:#F0B765; border-color:#F0B765; }
QPushButton:disabled, QToolButton:disabled { color:#6E7681; background:#161B22; border-color:#21262D; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit, QDateTimeEdit, QComboBox {
    background:#0D1117; color:#E6EDF3; border:1px solid #30363D; border-radius:6px; padding:5px 8px;
    selection-background-color:#493D2C; selection-color:#FFFFFF; }
QLineEdit:hover, QPlainTextEdit:hover, QTextEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover,
QDateEdit:hover, QTimeEdit:hover, QDateTimeEdit:hover, QComboBox:hover { border-color:#484F58; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QDateEdit:focus, QTimeEdit:focus, QDateTimeEdit:focus, QComboBox:focus { border-color:#E3A34A; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color:#6E7681; background:#161B22; }
QComboBox::drop-down { border:0; width:22px; }
QComboBox QAbstractItemView { background:#161B22; color:#E6EDF3; border:1px solid #30363D; outline:0; padding:4px;
    selection-background-color:#21262D; selection-color:#F0B765; }

QTabWidget::pane { border:1px solid #30363D; border-radius:8px; top:-1px; background:#0D1117; }
QTabBar::tab { background:transparent; color:#8B949E; padding:8px 14px; border:0; border-bottom:2px solid transparent; margin-right:2px; }
QTabBar::tab:hover { color:#E6EDF3; }
QTabBar::tab:selected { color:#E6EDF3; border-bottom:2px solid #E3A34A; }
QTabBar::tab:disabled { color:#6E7681; }

QHeaderView { background:#161B22; border:0; }
QHeaderView::section { background:#161B22; color:#8B949E; border:0; border-bottom:1px solid #30363D;
    border-right:1px solid #21262D; padding:6px 8px; font-weight:600; }
QTableCornerButton::section { background:#161B22; border:0; border-bottom:1px solid #30363D; }
QTableView, QTreeView, QListView { background:#0D1117; alternate-background-color:#11161D; color:#E6EDF3;
    border:1px solid #30363D; border-radius:8px; gridline-color:#21262D;
    selection-background-color:#493D2C; selection-color:#FFFFFF; outline:0; }
QListView::item { padding:4px 8px; min-height:24px; border-radius:6px; margin:1px 4px; }
QListView::item:hover { background:#161B22; }
QListView::item:selected { background:#493D2C; color:#FFFFFF; }

QScrollBar:vertical { background:transparent; width:12px; margin:0; }
QScrollBar:horizontal { background:transparent; height:12px; margin:0; }
QScrollBar::handle:vertical { background:#30363D; min-height:28px; border-radius:4px; margin:2px; }
QScrollBar::handle:horizontal { background:#30363D; min-width:28px; border-radius:4px; margin:2px; }
QScrollBar::handle:hover { background:#484F58; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; border:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }

QGroupBox { background:#161B22; border:1px solid #30363D; border-radius:10px; margin-top:16px; padding:14px 12px 12px 12px; }
QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left; left:12px; padding:0 6px; color:#8B949E; font-weight:600; }
QCheckBox, QRadioButton { spacing:8px; color:#E6EDF3; }

QProgressBar { background:#161B22; border:1px solid #30363D; border-radius:6px; text-align:center; color:#E6EDF3; min-height:14px; }
QProgressBar::chunk { background:#E3A34A; border-radius:5px; }
QSlider::groove:horizontal { height:4px; background:#30363D; border-radius:2px; }
QSlider::handle:horizontal { background:#E3A34A; width:14px; height:14px; margin:-5px 0; border-radius:7px; }

QSplitter::handle { background:#21262D; }
QSplitter::handle:hover { background:#E3A34A; }
QStatusBar { background:#0D1117; color:#8B949E; border-top:1px solid #21262D; }
QStatusBar::item { border:0; }
QDockWidget { color:#E6EDF3; }
QDockWidget::title { background:#161B22; padding:6px 10px; border-bottom:1px solid #30363D; }

/* ---- Variants: set with ui::set_variant(button, "primary"|"danger"|"quiet"|"buy"|"sell") ---- */
QPushButton:checked { background:#2D333B; color:#F0B765; border-color:#E3A34A; }
QPushButton[variant="primary"] { background:#E3A34A; color:#0D1117; border-color:#E3A34A; font-weight:600; }
QPushButton[variant="primary"]:hover { background:#F0B765; border-color:#F0B765; }
QPushButton[variant="danger"] { background:#DA3633; color:#FFFFFF; border-color:#DA3633; font-weight:600; }
QPushButton[variant="danger"]:hover { background:#E5534B; border-color:#E5534B; }
QPushButton[variant="quiet"] { background:transparent; color:#8B949E; border-color:transparent; }
QPushButton[variant="quiet"]:hover { background:#21262D; color:#E6EDF3; }
QPushButton[variant="quiet"]:checked { background:#2D333B; color:#F0B765; }
QPushButton[variant="buy"] { background:#1F6FEB; color:#FFFFFF; border-color:#1F6FEB; font-weight:700; }
QPushButton[variant="buy"]:hover { background:#388BFD; border-color:#388BFD; }
QPushButton[variant="sell"] { background:#DA3633; color:#FFFFFF; border-color:#DA3633; font-weight:700; }
QPushButton[variant="sell"]:hover { background:#E5534B; border-color:#E5534B; }
QPushButton[variant="primary"]:disabled, QPushButton[variant="danger"]:disabled,
QPushButton[variant="buy"]:disabled, QPushButton[variant="sell"]:disabled { background:#21262D; color:#6E7681; border-color:#30363D; }

/* The PAPER / LIVE switch. ui::set_variant(button, "paper"|"live"|"halted"). */
QPushButton[variant="paper"] { background:#12261A; color:#7EE787; border:1px solid #238636; font-weight:700; padding:5px 14px; }
QPushButton[variant="paper"]:hover { background:#173320; border-color:#3FB950; }
QPushButton[variant="live"] { background:#DA3633; color:#FFFFFF; border:1px solid #F85149; font-weight:700; padding:5px 14px; }
QPushButton[variant="halted"] { background:#6E1B1B; color:#FFFFFF; border:1px solid #DA3633; font-weight:700; padding:5px 14px; }

/* ---- Text roles: ui::set_role(label, "note"|"faint"|"error"|"warn"|"ok"|"info"|"kicker"|"title") ---- */
QLabel[role="note"] { color:#8B949E; }
QLabel[role="faint"] { color:#6E7681; }
QLabel[role="error"] { color:#F85149; }
QLabel[role="warn"] { color:#D29922; }
QLabel[role="ok"] { color:#3FB950; }
QLabel[role="info"] { color:#79C0FF; }
QLabel#sectionKicker, QLabel[role="kicker"] { color:#E3A34A; font-weight:700; letter-spacing:1px; }
QLabel[role="title"] { color:#E6EDF3; font-size:15px; font-weight:700; }
/* Status pills: ui::set_role(label, "pill-ok"|"pill-warn"|"pill-bad"|"pill-halt"). */
QLabel[role="pill-ok"] { color:#FFFFFF; background:#238636; font-weight:700; border-radius:9px; padding:1px 10px; }
QLabel[role="pill-warn"] { color:#FFFFFF; background:#9E6A03; font-weight:700; border-radius:9px; padding:1px 10px; }
QLabel[role="pill-bad"] { color:#FFFFFF; background:#DA3633; font-weight:700; border-radius:9px; padding:1px 10px; }
QLabel[role="pill-halt"] { color:#FFFFFF; background:#DA3633; font-weight:700; border-radius:9px; padding:1px 10px; }
/* Explanation banners above a panel: ui::set_role(label, "banner-info"|"banner-warn"). */
QLabel[role="banner-info"] { background:#0C2238; color:#C9E3FF; border:1px solid #1F4B7A; border-radius:8px; padding:10px 12px; }
QLabel[role="banner-warn"] { background:#2B2111; color:#F2DDB4; border:1px solid #6B4F1D; border-radius:8px; padding:10px 12px; }
/* Logs and reports: ui::set_role(edit, "log"). */
QPlainTextEdit[role="log"], QTextEdit[role="log"] { background:#0B0F14; color:#C9D1D9; border:1px solid #21262D;
    border-radius:8px; font-family:"Cascadia Mono","Consolas","JetBrains Mono",monospace; font-size:12px; padding:6px; }
/* A thin vertical rule between groups in a strip. */
QFrame[role="rule"] { background:#30363D; border:0; min-width:1px; max-width:1px; margin:4px 2px; }
)QSS");
}

[[nodiscard]] inline QPalette app_palette() {
    using namespace theme_token;
    QPalette p;
    const QColor canvas{kCanvas}, surface{kSurface}, alt{kSurfaceAlt}, raised{kRaised};
    const QColor text{kText}, faint{kTextFaint};
    p.setColor(QPalette::Window, canvas);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, canvas);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::ToolTipBase, raised);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, faint);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, raised);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor{Qt::white});
    p.setColor(QPalette::Light, QColor{kBorderHi});
    p.setColor(QPalette::Midlight, QColor{kBorder});
    p.setColor(QPalette::Mid, QColor{kBorder});
    p.setColor(QPalette::Dark, surface);
    p.setColor(QPalette::Shadow, QColor{"#010409"});
    p.setColor(QPalette::Highlight, QColor{kSelection});
    p.setColor(QPalette::HighlightedText, QColor{Qt::white});
    p.setColor(QPalette::Link, QColor{kLink});
    p.setColor(QPalette::LinkVisited, QColor{kLink});
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, faint);
    p.setColor(QPalette::Disabled, QPalette::Base, surface);
    p.setColor(QPalette::Disabled, QPalette::Button, surface);
    return p;
}

/// Re-apply the stylesheet to `w` after a dynamic property changed. Cheap: one
/// widget, not the whole window.
inline void repolish(QWidget* w) {
    if (w == nullptr) return;
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

/// Set a styling property and repolish only when it actually changed, so a
/// status refreshed every 500 ms costs nothing while it stays the same.
inline void set_style_property(QWidget* w, const char* name, const QString& value) {
    if (w == nullptr || w->property(name).toString() == value) return;
    w->setProperty(name, value);
    repolish(w);
}
inline void set_variant(QWidget* w, const QString& v) { set_style_property(w, "variant", v); }
inline void set_role(QWidget* w, const QString& r) { set_style_property(w, "role", r); }

/// setStyleSheet() re-polishes the widget and its children even when the
/// text is identical; this skips that when nothing changed.
inline void set_style_sheet_if_changed(QWidget* w, const QString& css) {
    if (w != nullptr && w->styleSheet() != css) w->setStyleSheet(css);
}

/// Pixel scrolling for every list and table: Qt's default jumps a whole row
/// per wheel notch, which reads as stutter on a long watch or option chain.
class SmoothScrolling final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Polish) {
            if (auto* v = qobject_cast<QAbstractItemView*>(o)) {
                v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                v->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            }
        }
        return false;
    }
};

/// Fusion + dark palette + stylesheet + a modern UI font stack.
inline void apply_app_theme(QApplication& app) {
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(fusion);
    QApplication::setPalette(app_palette());
    QFont font = QApplication::font();
    font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Segoe UI Variable Text"),
                      QStringLiteral("Segoe UI"), QStringLiteral("SF Pro Text"),
                      QStringLiteral("Helvetica Neue"), QStringLiteral("Arial")});
    if (font.pointSizeF() > 0 && font.pointSizeF() < 10.0) font.setPointSizeF(10.0);
    QApplication::setFont(font);
    app.setStyleSheet(app_style_sheet());
    app.installEventFilter(new SmoothScrolling(&app));
}

} // namespace altair::ui
