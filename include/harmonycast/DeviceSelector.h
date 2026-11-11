#pragma once

#include "DeviceCatalog.h"
#include <QComboBox>

class QFrame;
class QLineEdit;
class QListView;
class QLabel;
class QSortFilterProxyModel;

namespace harmonycast {

class DeviceSelector : public QComboBox {
    Q_OBJECT
public:
    static constexpr int SearchRole = Qt::UserRole + 1;
    static constexpr int PopupHeight = 352;

    explicit DeviceSelector(QWidget* parent = nullptr);
    ~DeviceSelector() override;
    void setEntries(const std::vector<DeviceListEntry>& entries, const QString& preferredSerial = {});
    void showPopup() override;
    void hidePopup() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void selectResult(const QModelIndex& index);
    void updateFilter(const QString& query);

    QFrame* m_popup;
    QLineEdit* m_search;
    QListView* m_results;
    QLabel* m_status;
    QSortFilterProxyModel* m_filter;
};

} // namespace harmonycast
