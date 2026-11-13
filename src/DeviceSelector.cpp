#include "harmonycast/DeviceSelector.h"
#include "harmonycast/IconManager.h"

#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QScreen>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

namespace harmonycast {
namespace {

class DeviceFilter : public QSortFilterProxyModel {
public:
    explicit DeviceFilter(QObject* parent) : QSortFilterProxyModel(parent) {}
    void setQuery(const QString& query) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_query = query;
        endFilterChange(Direction::Rows);
#else
        m_query = query;
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const auto* source = sourceModel();
        if (!source) return false;
        const auto index = source->index(row, 0, parent);
        if (!index.isValid()) return false;
        QString text = source->data(index, DeviceSelector::SearchRole).toString();
        if (text.isEmpty()) text = source->data(index, Qt::DisplayRole).toString();
        return DeviceCatalog::matchesSearchText(text, m_query);
    }

private:
    QString m_query;
};

} // namespace

DeviceSelector::DeviceSelector(QWidget* parent)
    : QComboBox(parent), m_popup(new QFrame(this, Qt::Popup)), m_search(new QLineEdit(m_popup)),
      m_results(new QListView(m_popup)), m_status(new QLabel(m_popup)), m_filter(new DeviceFilter(this)) {
    setObjectName("DeviceSelector");
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(18);
    setMaxVisibleItems(8);
    m_popup->setObjectName("DeviceSelectorPopup");
    m_popup->setFrameShape(QFrame::NoFrame);
    m_popup->setStyleSheet("QFrame#DeviceSelectorPopup { background: #e2e4e8; border: 1px solid #bec8d6; border-radius: 10px; }");
    auto* layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    m_search->setObjectName("DeviceSelectorSearch");
    m_search->setPlaceholderText("搜索 IP、端口、名称或状态");
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(32);
    m_search->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    layout->addWidget(m_search);
    m_filter->setSourceModel(model());
    m_results->setObjectName("DeviceSelectorResults");
    m_results->setModel(m_filter);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setUniformItemSizes(true);
    m_results->setTextElideMode(Qt::ElideMiddle);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_results->setStyleSheet("QListView { border: none; border-radius: 0px; padding: 0px; } QListView::item { min-height: 32px; padding: 2px 8px; border-radius: 6px; }");
    IconManager::setupScrollArea(m_results);
    m_results->installEventFilter(this);
    layout->addWidget(m_results, 1);
    m_status->setObjectName("Muted");
    m_status->setFixedHeight(20);
    layout->addWidget(m_status);
    connect(m_search, &QLineEdit::textChanged, this, &DeviceSelector::updateFilter);
    connect(m_results, &QListView::clicked, this, &DeviceSelector::selectResult);
    connect(m_results, &QListView::activated, this, &DeviceSelector::selectResult);
    auto refreshCount = [this] {
        const auto* source = m_filter->sourceModel();
        const int total = source ? source->rowCount() : 0;
        m_status->setText(m_filter->rowCount() == 0 ? QStringLiteral("没有匹配的设备") :
                         QString("匹配 %1 / %2 台").arg(m_filter->rowCount()).arg(total));
    };
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, refreshCount);
    connect(m_filter, &QAbstractItemModel::rowsRemoved, this, refreshCount);
    connect(m_filter, &QAbstractItemModel::modelReset, this, refreshCount);
}

DeviceSelector::~DeviceSelector() {
    // Remove proxy/view dependencies while the source model and all popup
    // children are still alive, before QComboBox starts its own teardown.
    m_search->removeEventFilter(this);
    m_results->removeEventFilter(this);
    disconnect(m_search, nullptr, this, nullptr);
    disconnect(m_results, nullptr, this, nullptr);
    disconnect(m_filter, nullptr, this, nullptr);
    m_popup->hide();
    m_results->setModel(nullptr);
    m_filter->setSourceModel(nullptr);
    delete m_popup;
    m_popup = nullptr;
    m_search = nullptr;
    m_results = nullptr;
    m_status = nullptr;
    delete m_filter;
    m_filter = nullptr;
}

void DeviceSelector::setEntries(const std::vector<DeviceListEntry>& entries, const QString& preferredSerial) {
    const QString selected = preferredSerial.isEmpty() ? currentData().toString() : preferredSerial;
    const QSignalBlocker blocker(this);
    if (m_filter->sourceModel() != model()) m_filter->setSourceModel(model());
    clear();
    int firstConnected = -1;
    for (const auto& entry : entries) {
        const int row = count();
        addItem(entry.label(), QString::fromStdString(entry.device.id()));
        setItemData(row, entry.searchText(), SearchRole);
        setItemData(row, entry.searchText(), Qt::ToolTipRole);
        setItemData(row, QSize(0, 36), Qt::SizeHintRole);
        if (firstConnected < 0 && entry.device.state == DeviceState::Connected) firstConnected = row;
    }
    const int preferred = findData(selected);
    setCurrentIndex(preferred >= 0 ? preferred : firstConnected >= 0 ? firstConnected : count() > 0 ? 0 : -1);
    if (m_popup->isVisible()) updateFilter(m_search->text());
}

void DeviceSelector::showPopup() {
    if (!isEnabled()) return;
    if (m_filter->sourceModel() != model()) m_filter->setSourceModel(model());
    m_popup->setPalette(palette());
    m_search->clear();
    updateFilter({});
    const auto selected = m_filter->mapFromSource(model()->index(currentIndex(), 0));
    if (selected.isValid()) {
        m_results->setCurrentIndex(selected);
        m_results->scrollTo(selected);
    }
    QScreen* activeScreen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    if (!activeScreen) activeScreen = screen();
    const QRect available = activeScreen ? activeScreen->availableGeometry().adjusted(8, 8, -8, -8) : QRect(0, 0, 1024, 768);
    const int popupWidth = std::min(std::max(width(), 360), available.width());
    const int popupHeight = std::min(PopupHeight, available.height());
    QPoint position = mapToGlobal(QPoint(0, height() + 4));
    if (position.y() + popupHeight > available.bottom()) position.setY(mapToGlobal(QPoint(0, 0)).y() - popupHeight - 4);
    position.setX(std::clamp(position.x(), available.left(), available.right() - popupWidth + 1));
    position.setY(std::clamp(position.y(), available.top(), available.bottom() - popupHeight + 1));
    m_popup->setFixedSize(popupWidth, popupHeight);
    m_popup->move(position);
    m_popup->show();
    m_search->setFocus(Qt::PopupFocusReason);
}

void DeviceSelector::hidePopup() {
    m_popup->hide();
    QComboBox::hidePopup();
}

void DeviceSelector::updateFilter(const QString& query) {
    static_cast<DeviceFilter*>(m_filter)->setQuery(query);
    m_results->setCurrentIndex(m_filter->index(0, 0));
    m_status->setText(m_filter->rowCount() == 0 ? QStringLiteral("没有匹配的设备") :
                     QString("匹配 %1 / %2 台").arg(m_filter->rowCount()).arg(count()));
}

void DeviceSelector::selectResult(const QModelIndex& index) {
    if (!m_popup->isVisible() || !index.isValid()) return;
    const int row = m_filter->mapToSource(index).row();
    if (row < 0 || row >= count()) return;
    // Activation is delivered only once, and only after an explicit selection.
    // MainWindow uses currentIndexChanged for state updates and activated for
    // re-selecting the same saved target, matching the QComboBox contract.
    hidePopup();
    setCurrentIndex(row);
    emit activated(row);
    setFocus(Qt::PopupFocusReason);
}

bool DeviceSelector::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_search || watched == m_results) && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) { hidePopup(); setFocus(); return true; }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            selectResult(m_results->currentIndex());
            return true;
        }
        if (watched == m_search && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            const int count = m_filter->rowCount();
            if (count > 0) {
                const int current = m_results->currentIndex().row();
                const int row = std::clamp(current + (key->key() == Qt::Key_Down ? 1 : -1), 0, count - 1);
                m_results->setCurrentIndex(m_filter->index(row, 0));
                m_results->scrollTo(m_results->currentIndex());
            }
            return true;
        }
    }
    return QComboBox::eventFilter(watched, event);
}

void DeviceSelector::wheelEvent(QWheelEvent* event) {
    // Scrolling the workbench must not silently switch the active device.
    event->ignore();
}

} // namespace harmonycast
