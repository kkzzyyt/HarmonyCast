#pragma once

#include <QIcon>
#include <QString>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QPalette>

namespace harmonycast {

class IconManager {
public:
    static QIcon getIcon(const QString& name);
    static QIcon makeSvgIcon(const QString& svgInner, const QString& stroke = "#8295a9", const QString& fill = "none", double strokeWidth = 2.0, int size = 18);
    static QPushButton* button(const QString& text, const QString& name = "", const QString& iconName = "");
    static QPushButton* iconButton(const QString& iconName, const QString& tooltip = "", const QString& name = "IconBtn", int width = 30, int height = 24);
    static QLabel* label(const QString& text, const QString& name = "", bool wrap = false);
    static void setupComboBoxPopup(QComboBox* combo, int minWidth = 0);
    static QPalette getDarkPalette();
    static QString getAppStyleSheet(const QString& resourceDir = "");
};

} // namespace harmonycast
