#include "../common/protocol.h"

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QClipboard>
#include <QMimeData>
#include <QTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QWheelEvent>
#include <QDateTime>
#include <QBasicTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QCoreApplication>
#include <QUrl>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <QStandardPaths>
#include <QDir>
#include <QHash>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeView>
#include <QSplitter>
#include <QFileSystemModel>
#include <QProgressBar>
#include <QEventLoop>
#include <QDialog>
#include <QPointer>
#include <QSet>
#include <QDirIterator>
#include <QStorageInfo>
#include <QSettings>
#include <QItemSelectionModel>
#include <QScreen>
#include <QGuiApplication>
#include <QStyle>
#include <QWindow>
#include <cmath>

// VK 常量（Windows Virtual Key Codes，与服务端协议一致）
#define VK_LSHIFT     0xA0
#define VK_RSHIFT     0xA1
#define VK_LCONTROL   0xA2
#define VK_RCONTROL   0xA3
#define VK_LMENU      0xA4
#define VK_RMENU      0xA5
#define VK_LWIN       0x5B
#define VK_RWIN       0x5C
#define VK_SPACE      0x20
#define VK_RETURN     0x0D
#define VK_BACK       0x08
#define VK_TAB        0x09
#define VK_ESCAPE     0x1B
#define VK_DELETE     0x2E
#define VK_INSERT     0x2D
#define VK_HOME       0x24
#define VK_END        0x23
#define VK_PRIOR      0x21
#define VK_NEXT       0x22
#define VK_LEFT       0x25
#define VK_RIGHT      0x27
#define VK_UP         0x26
#define VK_DOWN       0x28
#define VK_F1         0x70
#define VK_CAPITAL    0x14
#define VK_NUMLOCK    0x90
#define VK_SCROLL     0x91
#define VK_SNAPSHOT   0x2C
#define VK_PAUSE      0x13
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE

class RemoteWindow;
class FileTransferDialog;
class FloatingToolbar;

// ================================================================
// Qt::Key → Windows VK 映射（Linux 下 nativeVirtualKey 是 X11 keysym，不是 VK）
// ================================================================
static int qtKeyToVk(int k) {
    // 字母、数字直接对应 ASCII
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return k;
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return k;

    // F1..F12
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12)
        return VK_F1 + (k - Qt::Key_F1);

    switch (k) {
    case Qt::Key_Shift:      return VK_LSHIFT;
    case Qt::Key_Control:    return VK_LCONTROL;
    case Qt::Key_Alt:        return VK_LMENU;
    case Qt::Key_Meta:       return VK_LWIN;
    case Qt::Key_Return:
    case Qt::Key_Enter:      return VK_RETURN;
    case Qt::Key_Backspace:  return VK_BACK;
    case Qt::Key_Tab:        return VK_TAB;
    case Qt::Key_Escape:     return VK_ESCAPE;
    case Qt::Key_Space:      return VK_SPACE;
    case Qt::Key_Delete:     return VK_DELETE;
    case Qt::Key_Insert:     return VK_INSERT;
    case Qt::Key_Home:       return VK_HOME;
    case Qt::Key_End:        return VK_END;
    case Qt::Key_PageUp:     return VK_PRIOR;
    case Qt::Key_PageDown:   return VK_NEXT;
    case Qt::Key_Left:       return VK_LEFT;
    case Qt::Key_Right:      return VK_RIGHT;
    case Qt::Key_Up:         return VK_UP;
    case Qt::Key_Down:       return VK_DOWN;
    case Qt::Key_CapsLock:   return VK_CAPITAL;
    case Qt::Key_NumLock:    return VK_NUMLOCK;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Print:      return VK_SNAPSHOT;
    case Qt::Key_Pause:      return VK_PAUSE;
    case Qt::Key_Semicolon:  return VK_OEM_1;
    case Qt::Key_Equal:      return VK_OEM_PLUS;
    case Qt::Key_Comma:      return VK_OEM_COMMA;
    case Qt::Key_Minus:      return VK_OEM_MINUS;
    case Qt::Key_Period:     return VK_OEM_PERIOD;
    case Qt::Key_Slash:      return VK_OEM_2;
    case Qt::Key_QuoteLeft:  return VK_OEM_3;
    case Qt::Key_BracketLeft:  return VK_OEM_4;
    case Qt::Key_Backslash:    return VK_OEM_5;
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_Apostrophe:   return VK_OEM_7;
    default: return 0;
    }
}

// ================================================================
// RemoteWindow
// ================================================================
class RemoteWindow : public QWidget {
    Q_OBJECT
public:
    RemoteWindow(socket_t sock, int rw, int rh, const QString& profileName, QWidget* parent = nullptr);
    ~RemoteWindow() override;

    void sendCommand(const QString& cmd) { sendCmd(cmd); }
    void openFileTransfer();
    bool startDownload(const QString& localPath);
    void cancelDownload();
    bool downloadErrored() const { return m_dlError.load(); }
    void adaptToRemoteSize();      // 适应远端分辨率
    void toggleFullScreen();       // 全屏 / 退出全屏
signals:
    void disconnected();
    void downloadFinished(bool ok);
    void fsRootsReceived(const QByteArray& data);
    void uploadResult(bool ok);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void timerEvent(QTimerEvent* ev) override;
    bool event(QEvent* e) override;
    void changeEvent(QEvent* e) override;

private slots:
    void checkClipboard();
    void onClipboardChanged();
    void sendTargetResolution();

private:
    friend class FileTransferDialog;

    void onTileReceived(int x, int y, int w, int h, const QImage& tile);
    void onFullFrame(const QImage& img);
    QPoint toRemote(QPoint p);
    void flushMouse();
    int btnToId(Qt::MouseButton b);
    void sendCmd(const QString& cmd);
    void startReceiver();
    void stopReceiver();

    socket_t m_sock;
    int m_rw, m_rh;
    int m_origW, m_origH;
    QImage m_fullImage;
    double m_scale = 1.0;
    QPoint m_offset;
    QPoint m_pendingMouse;
    qint64 m_lastMouseSent = 0;
    QBasicTimer m_mouseTimer;
    QBasicTimer m_imeTimer;
    QString m_imeBuffer;
    std::mutex m_sendLock;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    QTimer* m_clipTimer;
    QTimer* m_resizeTimer;
    QString m_lastClipText;
    QFile* m_clipFile = nullptr;
    QString m_clipFilePath;
    qint64 m_clipFileRemaining = 0;
    QString m_lastClipFile;

    int m_lastSentTW = 0, m_lastSentTH = 0;

    std::atomic<FileTransferDialog*> m_ftDialog{nullptr};
    FloatingToolbar* m_toolbar = nullptr;
    std::mutex m_dlMutex;
    QFile* m_dlFile = nullptr;
    std::atomic<bool> m_dlError{false};
    std::atomic<bool> m_uploadWaiting{false};
    QRect m_normalGeometry;
};

// ================================================================
// FileTransferDialog
// ================================================================
class FileTransferDialog : public QDialog {
    Q_OBJECT
public:
    FileTransferDialog(RemoteWindow* remote, QWidget* parent = nullptr);
    ~FileTransferDialog() override;

    void onFsList(const QByteArray& data);

private slots:
    void uploadSelected();
    void downloadSelected();
    void onRemoteItemExpanded(QTreeWidgetItem* item);
    void onRemoteItemClicked(QTreeWidgetItem* item, int col);
    void loadRemoteRoot();
    void onLocalCurrentChanged(const QModelIndex& current,
                               const QModelIndex& previous);
private:
    enum {
        PathRole = Qt::UserRole,
        IsDirRole,
        LoadedRole,
    };
    enum class ConflictChoice { Overwrite, Skip, Cancel };
    enum class StatusKind { Info, Ok, Warn, Error };
    void setStatus(const QString& text, StatusKind kind = StatusKind::Info);
    ConflictChoice askConflict(const QString& targetName, bool allowSkip, bool* applyAll);
    bool remoteFileExists(const QString& remotePath);

    QByteArray requestDirListSync(const QString& path, bool* ok);
    void populateChildren(QTreeWidgetItem* parent, const QByteArray& data);
    bool collectRemoteFiles(const QString& remoteDir, const QString& relBase,
                            QList<QPair<QString, QString>>& out);
    bool uploadOneFile(const QString& localPath, const QString& remotePath);
    bool downloadOneFile(const QString& remotePath, const QString& localPath);
    QTreeWidgetItem* findItemByPath(const QString& path);
    void refreshRemoteItem(QTreeWidgetItem* item);
    void populateLocalRoots();
    QByteArray requestRemoteRoots();
    void loadRemoteRoots();
    void onLocalRootChanged(int idx);
    void onRemoteRootChanged(int idx);

    RemoteWindow* m_remote;
    QFileSystemModel* m_localModel = nullptr;
    QTreeView* m_localView = nullptr;
    QTreeWidget* m_remoteTree = nullptr;
    QLineEdit* m_localPathEdit = nullptr;
    QLineEdit* m_remotePathEdit = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;

    QEventLoop* m_waitLoop = nullptr;
    QByteArray m_dirListResult;
    bool m_dirListOk = false;
    QComboBox* m_localRootCombo  = nullptr;
    QComboBox* m_remoteRootCombo = nullptr;

    QByteArray m_remoteRootsResult;
    bool       m_remoteRootsOk = false;
    QHash<QString, QSet<QString>> m_remoteDirCache;
};
// ================================================================
// 自绘白色图标（两个平台统一，深色工具条上清晰可见）
// 如需改用 .rc / .qrc 资源图标，把 makeXxxIcon(...) 换成 QIcon(":/icons/xxx.png") 即可
// ================================================================
static void drawArrowHead(QPainter& p, QPointF from, QPointF to, qreal headSize) {
    const double angle = std::atan2(to.y() - from.y(), to.x() - from.x());
    const double a1 = angle + M_PI * 5.0 / 6.0;
    const double a2 = angle - M_PI * 5.0 / 6.0;
    QPointF p1 = to + QPointF(headSize * std::cos(a1), headSize * std::sin(a1));
    QPointF p2 = to + QPointF(headSize * std::cos(a2), headSize * std::sin(a2));
    QPolygonF tri;
    tri << to << p1 << p2;
    p.drawPolygon(tri);
}

// 文件传输：左右两条反向水平箭头（⇄）
static QIcon makeTransferIcon(const QColor& color, int s) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, s * 0.075, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(color);

    const qreal pad  = s * 0.16;
    const qreal left = pad, right = s - pad;
    const qreal yTop = s * 0.33, yBot = s * 0.67;
    const qreal hs   = s * 0.10;   // 箭头翼长

    // 上箭头：向右
    p.drawLine(QPointF(left, yTop), QPointF(right, yTop));
    drawArrowHead(p, QPointF(left, yTop), QPointF(right, yTop), hs);

    // 下箭头：向左
    p.drawLine(QPointF(right, yBot), QPointF(left, yBot));
    drawArrowHead(p, QPointF(right, yBot), QPointF(left, yBot), hs);

    p.end();
    return QIcon(pm);
}

// 适应：外方框 + 内部居中空心小方框（窗口套屏幕）
static QIcon makeAdaptIcon(const QColor& color, int s) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);

    const qreal pad = s * 0.16;
    QRectF outer(pad, pad, s - 2 * pad, s - 2 * pad);
    p.setPen(QPen(color, s * 0.075, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawRoundedRect(outer, 2.0, 2.0);

    QRectF inner = outer;
    inner.setSize(outer.size() * 0.5);
    inner.moveCenter(outer.center());
    p.setPen(QPen(color, s * 0.06));
    p.drawRoundedRect(inner, 1.5, 1.5);

    p.end();
    return QIcon(pm);
}

// 全屏 / 退出全屏
static QIcon makeFullscreenIcon(bool fullscreen, const QColor& color, int s) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, s * 0.075, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    const qreal pad = s * 0.16;
    const qreal len = s * 0.22;
    const qreal x1 = pad, x2 = s - pad;
    const qreal y1 = pad, y2 = s - pad;

    if (!fullscreen) {
        // 全屏：四角 L 形线（拐角在四角，臂朝内）
        p.drawLine(QPointF(x1, y1 + len), QPointF(x1, y1));
        p.drawLine(QPointF(x1, y1), QPointF(x1 + len, y1));
        p.drawLine(QPointF(x2 - len, y1), QPointF(x2, y1));
        p.drawLine(QPointF(x2, y1), QPointF(x2, y1 + len));
        p.drawLine(QPointF(x1, y2 - len), QPointF(x1, y2));
        p.drawLine(QPointF(x1, y2), QPointF(x1 + len, y2));
        p.drawLine(QPointF(x2 - len, y2), QPointF(x2, y2));
        p.drawLine(QPointF(x2, y2), QPointF(x2, y2 - len));
    } else {
        // 退出全屏：四角向内箭头
        const qreal d  = s * 0.10;
        const qreal a  = s * 0.20;
        const qreal hs = s * 0.09;

        QPointF p1(pad + d,           pad + d);
        QPointF q1(pad + d + a,       pad + d + a);
        p.drawLine(p1, q1); drawArrowHead(p, p1, q1, hs);

        QPointF p2(s - pad - d,       pad + d);
        QPointF q2(s - pad - d - a,   pad + d + a);
        p.drawLine(p2, q2); drawArrowHead(p, p2, q2, hs);

        QPointF p3(pad + d,           s - pad - d);
        QPointF q3(pad + d + a,       s - pad - d - a);
        p.drawLine(p3, q3); drawArrowHead(p, p3, q3, hs);

        QPointF p4(s - pad - d,       s - pad - d);
        QPointF q4(s - pad - d - a,   s - pad - d - a);
        p.drawLine(p4, q4); drawArrowHead(p, p4, q4, hs);
    }

    p.end();
    return QIcon(pm);
}
// ================================================================
// FloatingToolbar
// ================================================================
class FloatingToolbar : public QWidget {
    Q_OBJECT
public:
    FloatingToolbar(RemoteWindow* remote) : QWidget(remote), m_remote(remote) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                       | Qt::WindowDoesNotAcceptFocus
                       | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedHeight(56);

        // 工具栏深色背景改由 paintEvent 自绘（更可靠，任何主题下都与白色标题栏明显区分）
        // 样式表只负责三个按钮的透明 / hover / pressed 状态
        setStyleSheet(
            "QPushButton{background:transparent;border:none;border-radius:8px;}"
            "QPushButton:hover{background:rgba(255,255,255,50);}"
            "QPushButton:pressed{background:rgba(255,255,255,100);}");

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(4);

        const QColor iconColor = Qt::white;
        const int btnSize = 44;
        const QSize iconSize(26, 26);   // 图标 pixmap 32x32，显示 26x26，高 DPI 也清晰

        // ---- 文件传输 ----
        auto* transferBtn = new QPushButton(this);
        transferBtn->setCursor(Qt::PointingHandCursor);
        transferBtn->setFixedSize(btnSize, btnSize);
        transferBtn->setIconSize(iconSize);
        transferBtn->setIcon(makeTransferIcon(iconColor, 32));
        transferBtn->setToolTip(tr("文件传输"));
        lay->addWidget(transferBtn);

        // ---- 适应 ----
        auto* adaptBtn = new QPushButton(this);
        adaptBtn->setCursor(Qt::PointingHandCursor);
        adaptBtn->setFixedSize(btnSize, btnSize);
        adaptBtn->setIconSize(iconSize);
        adaptBtn->setIcon(makeAdaptIcon(iconColor, 32));
        adaptBtn->setToolTip(tr("适应（窗口大小 = 远端分辨率）"));
        lay->addWidget(adaptBtn);

        // ---- 全屏 ----
        m_fullBtn = new QPushButton(this);
        m_fullBtn->setCursor(Qt::PointingHandCursor);
        m_fullBtn->setFixedSize(btnSize, btnSize);
        m_fullBtn->setIconSize(iconSize);
        m_fullBtn->setIcon(makeFullscreenIcon(false, iconColor, 32));
        m_fullBtn->setToolTip(tr("全屏"));
        lay->addWidget(m_fullBtn);

        connect(transferBtn, &QPushButton::clicked, this, [this] {
            if (m_remote) m_remote->openFileTransfer();
        });
        connect(adaptBtn, &QPushButton::clicked, this, [this] {
            if (m_remote) m_remote->adaptToRemoteSize();
        });
        connect(m_fullBtn, &QPushButton::clicked, this, [this] {
            if (m_remote) m_remote->toggleFullScreen();
        });

        adjustSize();
    }

    // 全屏状态变化时由 RemoteWindow::changeEvent 调用
    void updateFullscreenIcon(bool fullscreen) {
        if (!m_fullBtn) return;
        m_fullBtn->setIcon(makeFullscreenIcon(fullscreen, Qt::white, 32));
        m_fullBtn->setToolTip(fullscreen ? tr("退出全屏") : tr("全屏"));
    }

    void updatePosition() {
        if (!m_remote) return;

        // 主窗口隐藏或最小化时同步隐藏
        if (!m_remote->isVisible() || m_remote->isMinimized()) {
            if (isVisible()) hide();
            return;
        }

        // ---- 全屏：贴屏幕上沿水平居中 ----
        if (m_remote->isFullScreen()) {
            // 兼容 Qt < 5.14（QWidget::screen() 是 5.14 才有的）
            QWindow* wh = m_remote->windowHandle();
            QScreen* s = wh ? wh->screen() : nullptr;
            if (!s) s = QGuiApplication::primaryScreen();
            if (!s) return;

            QRect sg = s->geometry();
            int x = sg.x() + (sg.width() - width()) / 2;
            int y = sg.y();
            move(x, y);
            if (!isVisible()) show();
            raise();
            return;
        }

        // ---- 普通窗口：贴标题栏水平居中（按钮尺寸固定，工具栏宽度保持不变） ----
        QRect fg = m_remote->frameGeometry();   // 含边框/标题栏
        QRect cg = m_remote->geometry();        // 客户区（都是屏幕坐标）
        int titleBarH = cg.top() - fg.top();    // 标题栏高度
        if (titleBarH <= 0) titleBarH = 30;     // 兜底

        int x = fg.x() + (fg.width() - width()) / 2;
        int y = fg.top() + titleBarH + 2;  // 垂直居中在标

        move(x, y);
        if (!isVisible()) show();
        raise();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // 深色半透明背景：能透出远程画面，同时保证白色图标清晰
        // 描边也改成半透明白，避免在半透明背景下形成生硬的黑框
        p.setPen(QPen(QColor(255, 255, 255, 70), 1.0));
        p.setBrush(QColor(15, 17, 22, 150));   // ← alpha 245 → 150
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    }
private:
    QPointer<RemoteWindow> m_remote;
    QPushButton* m_fullBtn = nullptr;
};

// ================================================================
// RemoteWindow 实现
// ================================================================
RemoteWindow::RemoteWindow(socket_t sock, int rw, int rh, const QString& profileName, QWidget* parent)
    : QWidget(parent), m_sock(sock), m_rw(rw), m_rh(rh), m_origW(rw), m_origH(rh) {
    setWindowTitle(QString("远程桌面 - %1x%2 - %3").arg(rw).arg(rh).arg(profileName));
    resize(1000, 700);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);

    m_fullImage = QImage(rw, rh, QImage::Format_ARGB32);
    m_fullImage.fill(Qt::black);

    m_clipTimer = new QTimer(this);
    connect(m_clipTimer, &QTimer::timeout, this, &RemoteWindow::checkClipboard);
    m_clipTimer->start(500);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(200);
    connect(m_resizeTimer, &QTimer::timeout, this, &RemoteWindow::sendTargetResolution);

    QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged,
                     this, &RemoteWindow::onClipboardChanged);

    m_lastClipText = QApplication::clipboard()->text();

    startReceiver();

    m_toolbar = new FloatingToolbar(this);
}

RemoteWindow::~RemoteWindow() {
    stopReceiver();
    FileTransferDialog* dlg = m_ftDialog.exchange(nullptr);
    if (dlg) { dlg->hide(); delete dlg; }
    if (m_toolbar) { m_toolbar->hide(); delete m_toolbar; m_toolbar = nullptr; }
    cancelDownload();
}
// ---------- 适应远端分辨率 ----------
void RemoteWindow::adaptToRemoteSize() {
    // 如果正处于全屏，先退出
    if (isFullScreen()) showNormal();

    // 兼容 Qt < 5.14
    QWindow* wh = windowHandle();
    QScreen* s = wh ? wh->screen() : nullptr;
    if (!s) s = QGuiApplication::primaryScreen();
    if (!s) return;
    const QRect avail = s->availableGeometry();

    // 用当前边框尺寸做补偿，让内容区接近远端分辨率
    int frameW = frameGeometry().width()  - width();
    int frameH = frameGeometry().height() - height();
    if (frameW < 0) frameW = 0;
    if (frameH < 0) frameH = 0;

    int targetW = std::min(m_origW + frameW, avail.width());
    int targetH = std::min(m_origH + frameH, avail.height());
    targetW = std::max(targetW, 320);
    targetH = std::max(targetH, 240);

    resize(targetW, targetH);

    // 在可用区域内居中
    int x = avail.x() + (avail.width()  - frameGeometry().width())  / 2;
    int y = avail.y() + (avail.height() - frameGeometry().height()) / 2;
    move(x, y);
}

// ---------- 全屏 / 退出全屏 ----------
void RemoteWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
        if (m_normalGeometry.isValid()) setGeometry(m_normalGeometry);
    } else {
        m_normalGeometry = geometry();
        showFullScreen();
    }
    // 状态变化会走 changeEvent，工具栏在那里统一更新
}

// ---------- 全屏状态变化事件 ----------
void RemoteWindow::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange) {
        if (m_toolbar) {
            m_toolbar->updateFullscreenIcon(isFullScreen());
            // 等窗口真正完成状态切换后再定位工具栏
            QTimer::singleShot(0, this, [this] {
                if (m_toolbar) m_toolbar->updatePosition();
            });
        }
    }
}
void RemoteWindow::openFileTransfer() {
    FileTransferDialog* dlg = m_ftDialog.load();
    if (dlg) { dlg->raise(); dlg->activateWindow(); return; }
    dlg = new FileTransferDialog(this);
    m_ftDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

bool RemoteWindow::startDownload(const QString& localPath) {
    std::lock_guard<std::mutex> lk(m_dlMutex);
    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
    m_dlFile = new QFile(localPath);
    m_dlError = false;
    if (!m_dlFile->open(QIODevice::WriteOnly)) {
        delete m_dlFile; m_dlFile = nullptr; return false;
    }
    return true;
}

void RemoteWindow::cancelDownload() {
    std::lock_guard<std::mutex> lk(m_dlMutex);
    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
}

void RemoteWindow::showEvent(QShowEvent* e) {
    QTimer::singleShot(100, this, &RemoteWindow::sendTargetResolution);
    QWidget::showEvent(e);
    if (m_toolbar) m_toolbar->updatePosition();
}

void RemoteWindow::hideEvent(QHideEvent* e) {
    if (m_toolbar) m_toolbar->hide();
    QWidget::hideEvent(e);
}

void RemoteWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (m_resizeTimer) m_resizeTimer->start();
    if (m_toolbar) m_toolbar->updatePosition();
}

void RemoteWindow::closeEvent(QCloseEvent* event) {
    if (m_running) {
        QMessageBox::StandardButton res = QMessageBox::question(this, "断开连接",
            "确定要断开远程桌面连接吗？", QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    stopReceiver();
    emit disconnected();
    event->accept();
}

bool RemoteWindow::event(QEvent* e) {
    bool ret = QWidget::event(e);
    if (m_toolbar && (e->type() == QEvent::Move || e->type() == QEvent::Resize)) {
        m_toolbar->updatePosition();
    }
    return ret;
}

void RemoteWindow::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setClipRect(event->rect());
    p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));
    if (m_fullImage.isNull()) return;

    QSize cw = size();
    QSize iw = m_fullImage.size();
    QSize scaled = iw.scaled(cw, Qt::KeepAspectRatio);
    m_offset = QPoint((cw.width() - scaled.width()) / 2,
                      (cw.height() - scaled.height()) / 2);
    m_scale = double(scaled.width()) / iw.width();

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(QRect(m_offset, scaled), m_fullImage);
}

void RemoteWindow::sendTargetResolution() {
    if (m_origW <= 0 || m_origH <= 0) return;

    double aspect = (double)m_origW / m_origH;
    QSize disp = QSize(m_origW, m_origH).scaled(size(), Qt::KeepAspectRatio);

    int tw = std::min({disp.width(), m_origW, 1920});
    tw = std::max(tw, 320);
    int th = (int)(tw / aspect);
    if (th > 1080) { th = 1080; tw = (int)(th * aspect); }
    if (th < 240) th = 240;

    if (tw == m_lastSentTW && th == m_lastSentTH) return;
    m_lastSentTW = tw;
    m_lastSentTH = th;
    sendCmd(QString("set_resolution %1 %2").arg(tw).arg(th));
}

void RemoteWindow::mouseMoveEvent(QMouseEvent* e) {
    QPoint r = toRemote(e->pos());
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_pendingMouse = r;
    if (now - m_lastMouseSent >= 50) {
        flushMouse();
    } else if (!m_mouseTimer.isActive()) {
        m_mouseTimer.start(50, this);
    }
}

void RemoteWindow::mousePressEvent(QMouseEvent* e) {
    flushMouse();
    sendCmd(QString("mousedown %1").arg(btnToId(e->button())));
}
void RemoteWindow::mouseReleaseEvent(QMouseEvent* e) {
    sendCmd(QString("mouseup %1").arg(btnToId(e->button())));
}
void RemoteWindow::wheelEvent(QWheelEvent* e) {
    sendCmd(e->angleDelta().y() > 0 ? "click 4" : "click 5");
}

void RemoteWindow::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;

    const int k = e->key();

    // ---- 修饰键：Linux 无全局钩子，直接用 Qt 的 keyPressEvent ----
    if (k == Qt::Key_Shift)   { sendCmd("keydown shiftleft");  return; }
    if (k == Qt::Key_Control) { sendCmd("keydown ctrl");       return; }
    if (k == Qt::Key_Alt)     { sendCmd("keydown alt");        return; }
    if (k == Qt::Key_Meta)    { sendCmd("keydown win");        return; }

    // ---- 中文输入法 / 多字节字符：走 paste 路径 ----
    QString t = e->text();
    if (!t.isEmpty() && t.at(0).isPrint() && t.at(0).unicode() > 127) {
        m_imeBuffer += t;
        if (m_imeTimer.isActive()) m_imeTimer.stop();
        m_imeTimer.start(300, this);
        return;
    }

    // ---- 组合键用 VK，普通字符走 type ----
    bool isCombo = (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));

    if (!t.isEmpty() && t.at(0).isPrint() && !isCombo) {
        sendCmd("type " + t);
        return;
    }

    int vk = qtKeyToVk(k);
    if (vk > 0) sendCmd(QString("keydown %1").arg(vk));
}

void RemoteWindow::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;

    const int k = e->key();

    // ---- 修饰键 ----
    if (k == Qt::Key_Shift)   { sendCmd("keyup shiftleft");  return; }
    if (k == Qt::Key_Control) { sendCmd("keyup ctrl");       return; }
    if (k == Qt::Key_Alt)     { sendCmd("keyup alt");        return; }
    if (k == Qt::Key_Meta)    { sendCmd("keyup win");        return; }

    // ---- 普通字符不重复发送（keyPressEvent 已发 type） ----
    QString t = e->text();
    bool isCombo = (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (!t.isEmpty() && t.at(0).isPrint() && !isCombo) return;

    int vk = qtKeyToVk(k);
    if (vk > 0) sendCmd(QString("keyup %1").arg(vk));
}

void RemoteWindow::timerEvent(QTimerEvent* ev) {
    if (ev->timerId() == m_mouseTimer.timerId()) {
        m_mouseTimer.stop();
        flushMouse();
    } else if (ev->timerId() == m_imeTimer.timerId()) {
        m_imeTimer.stop();
        if (!m_imeBuffer.isEmpty()) {
            QByteArray utf8 = m_imeBuffer.toUtf8();
            std::string b64 = proto::b64_encode(utf8.constData(), utf8.size());
            sendCmd(QString("paste %1").arg(QString::fromStdString(b64)));
            m_imeBuffer.clear();
        }
    }
}

void RemoteWindow::checkClipboard() {
    if (!isActiveWindow()) return;
    if (!QApplication::clipboard()->mimeData()) return;
    if (!QApplication::clipboard()->mimeData()->hasUrls()) return;

    QList<QUrl> urls = QApplication::clipboard()->mimeData()->urls();
    if (urls.isEmpty()) return;
    QString filePath = urls.first().toLocalFile();
    if (filePath.isEmpty() || !QFile::exists(filePath)) return;

    if (filePath == m_lastClipFile) return;
    m_lastClipFile = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return;
    QFileInfo fi(file);
    QString fileName = fi.fileName();
    qint64 fileSize = fi.size();

    QByteArray nameUtf8 = fileName.toUtf8();
    std::string nameB64 = proto::b64_encode(nameUtf8.constData(), nameUtf8.size());
    sendCmd(QString("file_begin %1 %2").arg(QString::fromStdString(nameB64)).arg(fileSize));

    while (!file.atEnd()) {
        QByteArray chunk = file.read(48 * 1024);
        std::string chunkB64 = proto::b64_encode(chunk.constData(), chunk.size());
        sendCmd("file_chunk " + QString::fromStdString(chunkB64));
    }
    sendCmd("file_end");
}

void RemoteWindow::onClipboardChanged() {
    if (QApplication::clipboard()->mimeData() &&
        QApplication::clipboard()->mimeData()->hasUrls()) {
        return;
    }

    QString text = QApplication::clipboard()->text();
    if (text.isEmpty() || text == m_lastClipText) return;
    m_lastClipText = text;

    QByteArray utf8 = text.toUtf8();
    std::string b64 = proto::b64_encode(utf8.constData(), utf8.size());
    sendCmd("clipboard " + QString::fromStdString(b64));
}

void RemoteWindow::onTileReceived(int x, int y, int w, int h, const QImage& tile) {
    QPainter p(&m_fullImage);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(x, y, tile);
    p.end();
    QRect vr(qRound(m_offset.x() + x * m_scale),
             qRound(m_offset.y() + y * m_scale),
             qRound(w * m_scale) + 1, qRound(h * m_scale) + 1);
    update(vr);
}

void RemoteWindow::onFullFrame(const QImage& img) {
    m_fullImage = img.copy();
    m_rw = img.width();
    m_rh = img.height();
    update();
}

QPoint RemoteWindow::toRemote(QPoint p) {
    if (m_scale <= 0 || m_rw <= 0 || m_rh <= 0) return QPoint(0, 0);
    int ix = int((p.x() - m_offset.x()) / m_scale);
    int iy = int((p.y() - m_offset.y()) / m_scale);
    int x = ix * m_origW / m_rw;
    int y = iy * m_origH / m_rh;
    return QPoint(std::clamp(x, 0, m_origW - 1), std::clamp(y, 0, m_origH - 1));
}

void RemoteWindow::flushMouse() {
    if (m_pendingMouse.isNull()) return;
    sendCmd(QString("mousemove %1 %2").arg(m_pendingMouse.x()).arg(m_pendingMouse.y()));
    m_pendingMouse = QPoint();
    m_lastMouseSent = QDateTime::currentMSecsSinceEpoch();
}

int RemoteWindow::btnToId(Qt::MouseButton b) {
    return b == Qt::LeftButton ? 1 : b == Qt::RightButton ? 3 : 2;
}

void RemoteWindow::sendCmd(const QString& cmd) {
    std::string s = cmd.toStdString() + "\n";
    std::lock_guard<std::mutex> lk(m_sendLock);
    send_all(m_sock, s.data(), s.size());
}

void RemoteWindow::startReceiver() {
    m_running = true;
    m_thread = std::thread([this] {
        while (m_running) {
            uint8_t hdr[5];
            if (!recv_exact(m_sock, hdr, 5)) break;
            uint8_t t = hdr[0];
            uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16)
                         | (uint32_t(hdr[3]) << 8) | uint32_t(hdr[4]);
            if (len > proto::MAX_FRAME) break;
            std::vector<uint8_t> buf(len);
            if (len && !recv_exact(m_sock, buf.data(), len)) break;

            if (t == proto::FRAME_SCREEN_DIFF && len >= 16) {
                uint32_t x, y, w, h;
                memcpy(&x, buf.data(), 4); x = ntohl(x);
                memcpy(&y, buf.data() + 4, 4); y = ntohl(y);
                memcpy(&w, buf.data() + 8, 4); w = ntohl(w);
                memcpy(&h, buf.data() + 12, 4); h = ntohl(h);
                QImage tile = QImage::fromData(buf.data() + 16, len - 16, "JPEG");
                if (!tile.isNull()) {
                    QMetaObject::invokeMethod(this, [this, x, y, w, h, tile]{
                        onTileReceived(x, y, w, h, tile);
                    }, Qt::QueuedConnection);
                }
            }
            else if (t == proto::FRAME_SCREEN) {
                QImage img = QImage::fromData(buf.data(), (int)len, "JPEG");
                if (!img.isNull()) {
                    QMetaObject::invokeMethod(this, [this, img]{ onFullFrame(img); }, Qt::QueuedConnection);
                }
            }
            else if (t == proto::FRAME_CLIPBOARD) {
                QString text = QString::fromUtf8((const char*)buf.data(), (int)len);
                QMetaObject::invokeMethod(this, [this, text]{
                    m_lastClipText = text;
                    QApplication::clipboard()->setText(text);
                }, Qt::QueuedConnection);
            }
            else if (t == proto::FRAME_FILE_BEGIN) {
                if (len < 12) break;
                uint32_t nameLen;
                memcpy(&nameLen, buf.data(), 4);
                nameLen = ntohl(nameLen);
                if (nameLen == 0 || nameLen > len - 12 || nameLen > 4096) break;

                uint64_t fileSize = 0;
                for (int i = 0; i < 8; ++i) fileSize = (fileSize << 8) | buf[4 + i];

                QString fileName = QString::fromUtf8((const char*)buf.data() + 12, (int)nameLen);
                fileName = QFileInfo(fileName).fileName();
                if (fileName.isEmpty()) fileName = "received_file";

                QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/remote_clip";
                QDir().mkpath(tmpDir);
                QString filePath = tmpDir + "/" + fileName;

                if (m_clipFile) {
                    m_clipFile->close();
                    delete m_clipFile;
                    m_clipFile = nullptr;
                }
                m_clipFile = new QFile(filePath);
                m_clipFile->open(QIODevice::WriteOnly);
                m_clipFilePath = filePath;
                m_clipFileRemaining = (qint64)fileSize;

                if (m_clipFileRemaining == 0) {
                    m_clipFile->close();
                    delete m_clipFile;
                    m_clipFile = nullptr;
                    QString finalPath = m_clipFilePath;
                    QMetaObject::invokeMethod(this, [this, finalPath] {
                        QMimeData* mime = new QMimeData;
                        QList<QUrl> urls;
                        urls << QUrl::fromLocalFile(finalPath);
                        mime->setUrls(urls);
                        mime->setText(finalPath);
                        QApplication::clipboard()->setMimeData(mime);
                        m_lastClipFile = finalPath;
                        m_lastClipText = finalPath;
                    }, Qt::QueuedConnection);
                }
            }
            else if (t == proto::FRAME_FILE_CHUNK) {
                if (m_clipFile && m_clipFileRemaining > 0) {
                    qint64 toWrite = std::min<qint64>((qint64)len, m_clipFileRemaining);
                    m_clipFile->write((const char*)buf.data(), toWrite);
                    m_clipFileRemaining -= toWrite;
                    if (m_clipFileRemaining <= 0) {
                        m_clipFile->close();
                        delete m_clipFile;
                        m_clipFile = nullptr;
                        QString finalPath = m_clipFilePath;
                        QMetaObject::invokeMethod(this, [this, finalPath] {
                            QMimeData* mime = new QMimeData;
                            QList<QUrl> urls;
                            urls << QUrl::fromLocalFile(finalPath);
                            mime->setUrls(urls);
                            mime->setText(finalPath);
                            QApplication::clipboard()->setMimeData(mime);
                            m_lastClipFile = finalPath;
                            m_lastClipText = finalPath;
                        }, Qt::QueuedConnection);
                    }
                }
            }
            else if (t == proto::FRAME_FS_LIST) {
                QByteArray data((const char*)buf.data(), (int)len);
                FileTransferDialog* dlg = m_ftDialog.load();
                if (dlg) {
                    QMetaObject::invokeMethod(this, [this, dlg, data]{
                        if (m_ftDialog.load() == dlg) dlg->onFsList(data);
                    }, Qt::QueuedConnection);
                }
            }
            else if (t == proto::FRAME_FS_ROOTS) {
                QByteArray data((const char*)buf.data(), (int)len);
                QMetaObject::invokeMethod(this, [this, data]{
                    emit fsRootsReceived(data);
                }, Qt::QueuedConnection);
            }
            else if (t == proto::FRAME_DL_BEGIN) {
                if (m_uploadWaiting.load()) emit uploadResult(true);
            }
            else if (t == proto::FRAME_DL_CHUNK) {
                std::lock_guard<std::mutex> lk(m_dlMutex);
                if (m_dlFile) m_dlFile->write((const char*)buf.data(), (qint64)len);
            }
            else if (t == proto::FRAME_DL_END) {
                bool ok;
                {
                    std::lock_guard<std::mutex> lk(m_dlMutex);
                    ok = (m_dlFile != nullptr) && !m_dlError.load();
                    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
                }
                emit downloadFinished(ok);
            }
            else if (t == proto::FRAME_FS_ERROR) {
                m_dlError = true;
                {
                    std::lock_guard<std::mutex> lk(m_dlMutex);
                    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }
                }
                if (m_uploadWaiting.load()) emit uploadResult(false);
                emit downloadFinished(false);
            }
        }

        // Linux 收尾：回到主线程 emit 信号并自删除（Windows 上走 close() 分支，这里不需要）
        if (m_running) {
            m_running = false;
            QMetaObject::invokeMethod(this, [this]{
                emit disconnected();
                this->deleteLater();
            }, Qt::QueuedConnection);
        }
    });
}

void RemoteWindow::stopReceiver() {
    m_running = false;
    sock_close(m_sock);
    if (m_thread.joinable()) m_thread.join();
}

// ================================================================
// FileTransferDialog 实现
// ================================================================
void FileTransferDialog::setStatus(const QString& text, StatusKind kind) {
    m_status->setText(text);
    switch (kind) {
    case StatusKind::Ok:
        m_status->setStyleSheet("color:#2e7d32;font-weight:bold;");
        break;
    case StatusKind::Warn:
        m_status->setStyleSheet("color:#ef6c00;");
        break;
    case StatusKind::Error:
        m_status->setStyleSheet("color:#c62828;font-weight:bold;");
        QApplication::beep();
        break;
    default:
        m_status->setStyleSheet("");
        break;
    }
}

FileTransferDialog::ConflictChoice
FileTransferDialog::askConflict(const QString& targetName, bool allowSkip, bool* applyAll) {
    if (applyAll) *applyAll = false;

    QMessageBox box(this);
    box.setWindowTitle("文件已存在");
    box.setIcon(QMessageBox::Question);
    box.setText("目标位置已存在同名文件：");
    box.setInformativeText(targetName);

    QPushButton* overwriteBtn = box.addButton("覆盖", QMessageBox::AcceptRole);
    QPushButton* skipBtn = nullptr;
    if (allowSkip)
        skipBtn = box.addButton("跳过", QMessageBox::ActionRole);
    box.addButton("取消", QMessageBox::RejectRole);

    QCheckBox* applyAllBox = nullptr;
    if (allowSkip) {
        applyAllBox = new QCheckBox("对全部冲突文件应用此选择", &box);
        box.setCheckBox(applyAllBox);
    }
    box.setDefaultButton(overwriteBtn);
    box.exec();

    if (applyAll && applyAllBox && applyAllBox->isChecked())
        *applyAll = true;

    QAbstractButton* clicked = box.clickedButton();
    if (clicked == overwriteBtn) return ConflictChoice::Overwrite;
    if (skipBtn && clicked == skipBtn) return ConflictChoice::Skip;
    return ConflictChoice::Cancel;
}

bool FileTransferDialog::remoteFileExists(const QString& remotePath) {
    const QString dir  = QFileInfo(remotePath).path();
    const QString name = QFileInfo(remotePath).fileName();
    if (name.isEmpty() || dir.isEmpty()) return false;

    // 远端是 Windows 就大小写不敏感（盘符形如 C:/）
    const bool winLike = (dir.size() >= 2 && dir[1] == ':');
    auto norm = [winLike](const QString& s) { return winLike ? s.toLower() : s; };

    if (!m_remoteDirCache.contains(dir)) {
        QSet<QString> names;
        bool ok = false;
        QByteArray data = requestDirListSync(dir, &ok);
        if (ok) {
            const QList<QByteArray> lines = data.split('\n');
            for (int i = 1; i < lines.size(); ++i) {
                QByteArray line = lines[i];
                if (line.endsWith('\r')) line.chop(1);
                if (line.isEmpty()) continue;
                const QList<QByteArray> parts = line.split('\t');
                if (parts.size() < 3) continue;
                QString abs = QString::fromStdString(proto::b64_decode(parts[1].toStdString()));
                names.insert(norm(QFileInfo(abs).fileName()));
            }
        }
        m_remoteDirCache.insert(dir, names);
    }
    return m_remoteDirCache.value(dir).contains(norm(name));
}

FileTransferDialog::FileTransferDialog(RemoteWindow* remote, QWidget* parent)
    : QDialog(parent), m_remote(remote) {
    setWindowTitle("文件传输");
    resize(1100, 680);

    auto* mainLay = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // ---- 左：本地 ----
    auto* leftWidget = new QWidget(this);
    auto* leftLay = new QVBoxLayout(leftWidget);
    leftLay->setContentsMargins(0, 0, 0, 0);

    auto* localHeader = new QHBoxLayout();
    localHeader->addWidget(new QLabel("本地根:", leftWidget));
    m_localRootCombo = new QComboBox(leftWidget);
    m_localRootCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    localHeader->addWidget(m_localRootCombo, 1);
    leftLay->addLayout(localHeader);

    m_localModel = new QFileSystemModel(this);
    m_localModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    m_localView = new QTreeView(leftWidget);
    m_localView->setModel(m_localModel);
    m_localView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_localView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_localView->setColumnWidth(0, 300);
    m_localView->setColumnHidden(1, true);
    m_localView->setColumnHidden(2, true);
    m_localView->setColumnHidden(3, true);
    leftLay->addWidget(m_localView, 1);

    connect(m_localView->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, &FileTransferDialog::onLocalCurrentChanged);

    m_localPathEdit = new QLineEdit(QDir::homePath(), leftWidget);
    m_localPathEdit->setPlaceholderText("下载保存目录");
    leftLay->addWidget(m_localPathEdit);

    connect(m_localRootCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileTransferDialog::onLocalRootChanged);
    populateLocalRoots();

    // ---- 右：远端 ----
    auto* rightWidget = new QWidget(this);
    auto* rightLay    = new QVBoxLayout(rightWidget);
    rightLay->setContentsMargins(0, 0, 0, 0);

    auto* remoteHeader = new QHBoxLayout();
    remoteHeader->addWidget(new QLabel("远端根:", rightWidget));
    m_remoteRootCombo = new QComboBox(rightWidget);
    m_remoteRootCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    remoteHeader->addWidget(m_remoteRootCombo, 1);
    rightLay->addLayout(remoteHeader);

    m_remoteTree = new QTreeWidget(rightWidget);
    m_remoteTree->setColumnCount(2);
    m_remoteTree->setHeaderLabels({"名称", "大小"});
    m_remoteTree->setColumnWidth(0, 350);
    m_remoteTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rightLay->addWidget(m_remoteTree, 1);

    m_remotePathEdit = new QLineEdit(rightWidget);
    m_remotePathEdit->setPlaceholderText("上传目标目录（点击远端目录更新）");
    rightLay->addWidget(m_remotePathEdit);

    connect(m_remoteRootCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileTransferDialog::onRemoteRootChanged);

    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    mainLay->addWidget(splitter, 1);

    auto* btnLay = new QHBoxLayout();
    auto* uploadBtn   = new QPushButton("上传 →  (本地选中的文件/文件夹 传到 远端当前目录)", this);
    auto* downloadBtn = new QPushButton("← 下载  (远端选中的文件/文件夹 保存到 本地目录)", this);
    btnLay->addWidget(uploadBtn, 1);
    btnLay->addWidget(downloadBtn, 1);
    mainLay->addLayout(btnLay);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    mainLay->addWidget(m_progress);

    m_status = new QLabel("就绪", this);
    mainLay->addWidget(m_status);

    connect(uploadBtn,   &QPushButton::clicked, this, &FileTransferDialog::uploadSelected);
    connect(downloadBtn, &QPushButton::clicked, this, &FileTransferDialog::downloadSelected);
    connect(m_remoteTree, &QTreeWidget::itemExpanded, this, &FileTransferDialog::onRemoteItemExpanded);
    connect(m_remoteTree, &QTreeWidget::itemClicked,  this, &FileTransferDialog::onRemoteItemClicked);

    QTimer::singleShot(0, this, &FileTransferDialog::loadRemoteRoots);
}

FileTransferDialog::~FileTransferDialog() {
    if (m_waitLoop) m_waitLoop->quit();
    if (m_remote && m_remote->m_ftDialog.load() == this)
        m_remote->m_ftDialog = nullptr;
}

void FileTransferDialog::onFsList(const QByteArray& data) {
    if (m_waitLoop) {
        m_dirListResult = data;
        m_dirListOk = true;
        m_waitLoop->quit();
    }
}

void FileTransferDialog::onLocalCurrentChanged(const QModelIndex& current,
                                               const QModelIndex& /*previous*/) {
    if (!current.isValid()) return;

    QFileInfo fi = m_localModel->fileInfo(current);
    if (!fi.exists()) return;

    QString target = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (target.isEmpty()) return;
    if (target == m_localPathEdit->text()) return;

    m_localPathEdit->setText(target);
    QSettings("RemoteDesktop", "Client").setValue("lastDownloadDir", target);
}

QByteArray FileTransferDialog::requestDirListSync(const QString& path, bool* ok) {
    m_dirListResult.clear();
    m_dirListOk = false;

    QByteArray pathUtf8 = path.toUtf8();
    std::string b64 = proto::b64_encode(pathUtf8.constData(), pathUtf8.size());
    m_remote->sendCommand("fs_list " + QString::fromStdString(b64));

    QEventLoop loop;
    m_waitLoop = &loop;
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    loop.exec();
    m_waitLoop = nullptr;
    if (ok) *ok = m_dirListOk;
    return m_dirListResult;
}

void FileTransferDialog::populateLocalRoots() {
    m_localRootCombo->blockSignals(true);
    m_localRootCombo->clear();

#ifdef Q_OS_WIN
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady()) continue;
        QString label = v.displayName().isEmpty()
                        ? v.rootPath()
                        : QString("%1 (%2)").arg(v.displayName(), v.rootPath());
        m_localRootCombo->addItem(label, v.rootPath());
    }
    if (m_localRootCombo->count() == 0)
        m_localRootCombo->addItem("C:\\", "C:\\");
#else
    // UOS / Linux：/ 以及 / 下的一级子目录 + home
    m_localRootCombo->addItem("/", "/");
    QDir rootDir("/");
    for (const QString& d : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QString p = "/" + d;
        m_localRootCombo->addItem(p, p);
    }
    const QString home = QDir::homePath();
    if (home != "/" && m_localRootCombo->findData(home) < 0)
        m_localRootCombo->addItem(home, home);
#endif

    m_localRootCombo->blockSignals(false);

    QSettings settings("RemoteDesktop", "Client");
    QString last = settings.value("lastLocalRoot").toString();
    int idx = m_localRootCombo->findData(last);
    if (idx < 0) idx = m_localRootCombo->findData(QDir::homePath());
    if (idx < 0) idx = 0;

    QString lastDl = settings.value("lastDownloadDir").toString();
    QString initDl = !lastDl.isEmpty()
                     ? lastDl
                     : (idx >= 0 ? m_localRootCombo->itemData(idx).toString()
                                 : QDir::homePath());
    m_localPathEdit->setText(initDl);

    m_localRootCombo->setCurrentIndex(idx);
    onLocalRootChanged(idx);
}

void FileTransferDialog::onLocalRootChanged(int idx) {
    if (idx < 0) return;
    QString root = m_localRootCombo->itemData(idx).toString();
    if (root.isEmpty()) return;
    m_localModel->setRootPath(root);
    m_localView->setRootIndex(m_localModel->index(root));
    QSettings("RemoteDesktop", "Client").setValue("lastLocalRoot", root);
}

QByteArray FileTransferDialog::requestRemoteRoots() {
    m_remoteRootsResult.clear();
    m_remoteRootsOk = false;
    m_remote->sendCommand("fs_roots");

    QEventLoop loop;
    QMetaObject::Connection conn = connect(
        m_remote, &RemoteWindow::fsRootsReceived,
        &loop, [this, &loop](const QByteArray& data) {
            m_remoteRootsResult = data;
            m_remoteRootsOk = true;
            loop.quit();
        });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(conn);
    return m_remoteRootsOk ? m_remoteRootsResult : QByteArray();
}

void FileTransferDialog::loadRemoteRoots() {
    QByteArray data = requestRemoteRoots();
    if (data.isEmpty()) {
        m_status->setText("无法获取远端根目录，请检查连接");
        return;
    }

    m_remoteRootCombo->blockSignals(true);
    m_remoteRootCombo->clear();

    const QList<QByteArray> lines = data.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        QString p = QString::fromStdString(proto::b64_decode(line.toStdString()));
        if (p.isEmpty()) continue;
        m_remoteRootCombo->addItem(p, p);
    }
    m_remoteRootCombo->blockSignals(false);

    if (m_remoteRootCombo->count() == 0) {
        setStatus("远端未返回根目录", StatusKind::Error);
        return;
    }

    QSettings settings("RemoteDesktop", "Client");
    QString last = settings.value("lastRemoteRoot").toString();
    int idx = m_remoteRootCombo->findData(last);
    if (idx < 0) idx = 0;
    m_remoteRootCombo->setCurrentIndex(idx);

    onRemoteRootChanged(idx);
    m_status->setText("已连接远端");
}

void FileTransferDialog::onRemoteRootChanged(int idx) {
    if (idx < 0) return;
    QString root = m_remoteRootCombo->itemData(idx).toString();
    if (root.isEmpty()) return;

    bool ok = false;
    QByteArray data = requestDirListSync(root, &ok);
    if (!ok || data.isEmpty()) {
        setStatus("加载远端根目录失败: " + root, StatusKind::Error);
        return;
    }
    m_remoteTree->invisibleRootItem()->takeChildren();
    populateChildren(m_remoteTree->invisibleRootItem(), data);
    m_remotePathEdit->setText(root);
    QSettings("RemoteDesktop", "Client").setValue("lastRemoteRoot", root);
}

void FileTransferDialog::loadRemoteRoot() {
    bool ok = false;
    QByteArray data = requestDirListSync("", &ok);
    if (!ok || data.isEmpty()) {
        setStatus("无法获取远端根目录，请检查连接", StatusKind::Error);
        return;
    }
    QList<QByteArray> lines = data.split('\n');
    if (lines.isEmpty()) return;
    QByteArray first = lines[0];
    if (first.endsWith('\r')) first.chop(1);
    QString rootPath = QString::fromUtf8(first.constData(), first.size());
    m_remotePathEdit->setText(rootPath);

    m_remoteTree->invisibleRootItem()->takeChildren();
    populateChildren(m_remoteTree->invisibleRootItem(), data);
    setStatus("已连接远端", StatusKind::Ok);
}

void FileTransferDialog::populateChildren(QTreeWidgetItem* parent, const QByteArray& data) {
    parent->takeChildren();
    QList<QByteArray> lines = data.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        QList<QByteArray> parts = line.split('\t');
        if (parts.size() < 3) continue;
        char type = parts[0].isEmpty() ? 'f' : parts[0][0];
        QString abs = QString::fromStdString(proto::b64_decode(parts[1].toStdString()));
        QString name = QFileInfo(abs).fileName();
        if (name.isEmpty()) name = abs;
        qint64 size = parts[2].toLongLong();

        auto* child = new QTreeWidgetItem(parent);
        child->setText(0, name + (type == 'd' ? "/" : ""));
        if (type == 'f')
            child->setText(1, QString::number(size));
        child->setData(0, PathRole, abs);
        child->setData(0, IsDirRole, type == 'd');
        child->setData(0, LoadedRole, false);
        if (type == 'd')
            child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

void FileTransferDialog::onRemoteItemExpanded(QTreeWidgetItem* item) {
    if (!item) return;
    if (!item->data(0, IsDirRole).toBool()) return;
    if (item->data(0, LoadedRole).toBool()) return;

    item->setData(0, LoadedRole, true);
    bool ok = false;
    QByteArray data = requestDirListSync(item->data(0, PathRole).toString(), &ok);
    if (ok) populateChildren(item, data);
    else item->setData(0, LoadedRole, false);
}

void FileTransferDialog::onRemoteItemClicked(QTreeWidgetItem* item, int) {
    if (!item) return;
    if (item->data(0, IsDirRole).toBool())
        m_remotePathEdit->setText(item->data(0, PathRole).toString());
}

QTreeWidgetItem* FileTransferDialog::findItemByPath(const QString& path) {
    QList<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_remoteTree->topLevelItemCount(); ++i)
        stack.append(m_remoteTree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, PathRole).toString() == path) return it;
        for (int i = 0; i < it->childCount(); ++i)
            stack.append(it->child(i));
    }
    return nullptr;
}

void FileTransferDialog::refreshRemoteItem(QTreeWidgetItem* item) {
    if (!item) return;
    item->setData(0, LoadedRole, true);
    bool ok = false;
    QByteArray data = requestDirListSync(item->data(0, PathRole).toString(), &ok);
    if (ok) populateChildren(item, data);
    else item->setData(0, LoadedRole, false);
}

bool FileTransferDialog::uploadOneFile(const QString& localPath, const QString& remotePath) {
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    qint64 size = f.size();

    QByteArray pb = remotePath.toUtf8();
    std::string pb64 = proto::b64_encode(pb.constData(), pb.size());

    bool ackReceived = false;
    bool ackOk       = false;
    m_remote->m_uploadWaiting = true;
    {
        QEventLoop loop;
        QMetaObject::Connection conn = connect(
            m_remote, &RemoteWindow::uploadResult,
            &loop, [&](bool ok) {
                ackReceived = true;
                ackOk       = ok;
                loop.quit();
            });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);

        m_remote->sendCommand(QString("fs_put_begin %1 %2")
                              .arg(QString::fromStdString(pb64)).arg(size));
        loop.exec();
        disconnect(conn);
    }
    m_remote->m_uploadWaiting = false;

    if (!ackReceived || !ackOk) return false;

    while (!f.atEnd()) {
        QByteArray chunk = f.read(48 * 1024);
        if (chunk.isEmpty()) break;
        std::string cb64 = proto::b64_encode(chunk.constData(), chunk.size());
        m_remote->sendCommand("fs_put_chunk " + QString::fromStdString(cb64));
    }
    m_remote->sendCommand("fs_put_end");
    return true;
}

void FileTransferDialog::uploadSelected() {
    QString targetDir = m_remotePathEdit->text().trimmed();
    if (targetDir.isEmpty()) {
        setStatus("请先在右侧选择远端目标目录", StatusKind::Error);
        return;
    }

    QModelIndexList idxs = m_localView->selectionModel()->selectedIndexes();
    QSet<QString> paths;
    for (const auto& idx : idxs)
        if (idx.column() == 0) paths.insert(m_localModel->filePath(idx));
    if (paths.isEmpty()) { setStatus("请先在左侧选择要上传的文件或文件夹", StatusKind::Error); return; }

    QStringList roots;
    for (const auto& p : paths) {
        bool isChild = false;
        for (const auto& q : paths) {
            if (q != p && p.startsWith(q + "/")) { isChild = true; break; }
        }
        if (!isChild) roots << p;
    }

    QList<QPair<QString, QString>> files;
    for (const auto& root : roots) {
        QFileInfo fi(root);
        if (fi.isDir()) {
            QString baseName = fi.fileName();
            if (baseName.isEmpty()) baseName = root.section('/', -1);
            QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QString localAbs = it.filePath();
                QString rel = QDir(root).relativeFilePath(localAbs);
                files.append({localAbs, baseName + "/" + rel});
            }
        } else {
            files.append({root, fi.fileName()});
        }
    }

    if (files.isEmpty()) { setStatus("没有可上传的文件", StatusKind::Error); return; }
    m_remoteDirCache.clear();
    m_progress->setVisible(true);
    m_progress->setRange(0, files.size());
    m_progress->setValue(0);
    const bool allowSkip = (files.size() > 1);
    bool globalSet = false;
    ConflictChoice globalChoice = ConflictChoice::Overwrite;

    int done = 0;
    for (const auto& f : files) {
        QString remoteTarget = QDir(targetDir).filePath(f.second);

        if (remoteFileExists(remoteTarget)) {
            ConflictChoice c;
            if (globalSet) {
                c = globalChoice;
            } else {
                bool applyAll = false;
                c = askConflict(QFileInfo(remoteTarget).fileName(), allowSkip, &applyAll);
                if (applyAll) { globalSet = true; globalChoice = c; }
            }

            if (c == ConflictChoice::Cancel) {
                setStatus(QString("已取消，剩余 %1 个文件未上传")
                          .arg(files.size() - done), StatusKind::Warn);
                m_progress->setVisible(false);
                return;
            }
            if (c == ConflictChoice::Skip) {
                m_progress->setValue(++done);
                setStatus(QString("已跳过 %1/%2").arg(done).arg(files.size()), StatusKind::Warn);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                continue;
            }
        }

        if (!uploadOneFile(f.first, remoteTarget)) {
            setStatus(QString("上传失败（远端无写入权限或路径无效）: %1 → %2")
                      .arg(f.first, remoteTarget), StatusKind::Error);
            m_progress->setVisible(false);
            return;
        }
        m_progress->setValue(++done);
        setStatus(QString("上传中 %1/%2").arg(done).arg(files.size()));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }

    setStatus(QString("上传完成，共 %1 个文件").arg(files.size()), StatusKind::Ok);
    m_progress->setVisible(false);

    QTreeWidgetItem* targetItem = findItemByPath(targetDir);
    if (targetItem) refreshRemoteItem(targetItem);
}

bool FileTransferDialog::collectRemoteFiles(const QString& remoteDir, const QString& relBase,
                                            QList<QPair<QString, QString>>& out) {
    bool ok = false;
    QByteArray data = requestDirListSync(remoteDir, &ok);
    if (!ok) return false;

    QList<QByteArray> lines = data.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        QList<QByteArray> parts = line.split('\t');
        if (parts.size() < 3) continue;
        char type = parts[0].isEmpty() ? 'f' : parts[0][0];
        QString abs = QString::fromStdString(proto::b64_decode(parts[1].toStdString()));
        QString name = QFileInfo(abs).fileName();
        QString rel = relBase.isEmpty() ? name : relBase + "/" + name;
        if (type == 'd') {
            if (!collectRemoteFiles(abs, rel, out)) return false;
        } else {
            out.append({abs, rel});
        }
    }
    return true;
}

bool FileTransferDialog::downloadOneFile(const QString& remotePath, const QString& localPath) {
    if (!m_remote->startDownload(localPath)) return false;

    QByteArray pb = remotePath.toUtf8();
    std::string b64 = proto::b64_encode(pb.constData(), pb.size());

    QEventLoop loop;
    QMetaObject::Connection conn = connect(m_remote, &RemoteWindow::downloadFinished,
                                           &loop, &QEventLoop::quit);
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);

    m_remote->sendCommand("fs_get " + QString::fromStdString(b64));
    loop.exec();
    disconnect(conn);

    if (m_remote->downloadErrored()) {
        m_remote->cancelDownload();
        return false;
    }
    m_remote->cancelDownload();
    return true;
}

void FileTransferDialog::downloadSelected() {
    QString localDir = m_localPathEdit->text().trimmed();
    if (localDir.isEmpty()) {
        setStatus("请先在左侧选择本地保存目录", StatusKind::Error);
        return;
    }
    QDir().mkpath(localDir);

    QList<QTreeWidgetItem*> sel = m_remoteTree->selectedItems();
    if (sel.isEmpty()) { setStatus("请先在右侧选择要下载的文件或文件夹", StatusKind::Error); return; }

    struct RootItem { QString path; bool isDir; };
    QList<RootItem> selItems;
    for (auto* it : sel) {
        selItems.append({it->data(0, PathRole).toString(), it->data(0, IsDirRole).toBool()});
    }
    QList<RootItem> roots;
    for (int i = 0; i < selItems.size(); ++i) {
        bool isChild = false;
        for (int j = 0; j < selItems.size(); ++j) {
            if (i == j) continue;
            if (selItems[i].path.startsWith(selItems[j].path + "/")) { isChild = true; break; }
        }
        if (!isChild) roots.append(selItems[i]);
    }

    QList<QPair<QString, QString>> files;
    for (const auto& r : roots) {
        if (r.isDir) {
            QString base = r.path.section('/', -1);
            if (base.isEmpty()) base = "dir";
            if (!collectRemoteFiles(r.path, base, files)) {
                setStatus("获取远端目录失败", StatusKind::Error);
                return;
            }
        } else {
            files.append({r.path, r.path.section('/', -1)});
        }
    }

    if (files.isEmpty()) { setStatus("没有可下载的文件", StatusKind::Error); return; }

    m_progress->setVisible(true);
    m_progress->setRange(0, files.size());
    m_progress->setValue(0);
    const bool allowSkip = (files.size() > 1);
    bool globalSet = false;
    ConflictChoice globalChoice = ConflictChoice::Overwrite;

    int done = 0;
    for (const auto& f : files) {
        QString localTarget = QDir(localDir).filePath(f.second);
        QDir().mkpath(QFileInfo(localTarget).path());

        if (QFile::exists(localTarget)) {
            ConflictChoice c;
            if (globalSet) {
                c = globalChoice;
            } else {
                bool applyAll = false;
                c = askConflict(QFileInfo(localTarget).fileName(), allowSkip, &applyAll);
                if (applyAll) { globalSet = true; globalChoice = c; }
            }

            if (c == ConflictChoice::Cancel) {
                setStatus(QString("已取消，剩余 %1 个文件未下载")
                          .arg(files.size() - done), StatusKind::Warn);
                m_progress->setVisible(false);
                return;
            }
            if (c == ConflictChoice::Skip) {
                m_progress->setValue(++done);
                setStatus(QString("已跳过 %1/%2").arg(done).arg(files.size()), StatusKind::Warn);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                continue;
            }
        }

        if (!downloadOneFile(f.first, localTarget)) {
            setStatus("下载失败: " + f.first, StatusKind::Error);
            m_progress->setVisible(false);
            return;
        }
        m_progress->setValue(++done);
        setStatus(QString("下载中 %1/%2").arg(done).arg(files.size()), StatusKind::Info);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }

    setStatus(QString("下载完成，共 %1 个文件").arg(files.size()), StatusKind::Ok);
    QSettings("RemoteDesktop", "Client").setValue("lastDownloadDir", localDir);
    m_progress->setVisible(false);
}

// ================================================================
// LoginWindow
// ================================================================
class LoginWindow : public QWidget {
    Q_OBJECT
public:
    LoginWindow();

private slots:
    void onProfileSelected(const QString& name);
    void tryConnect();

private:
    QString configPath();
    void loadConfig();
    void saveConfig(const QString& name, const QString& ip, const QString& port,
                    bool remember, const QString& pwd);

    QComboBox* m_nameCombo;
    QLineEdit *m_ip, *m_port, *m_pwd;
    QPushButton* m_togglePwdBtn;
    QCheckBox* m_remember;
    QLabel* m_status;
};

LoginWindow::LoginWindow() {
    setWindowTitle("远程桌面控制端");
    QIcon appIcon;
    appIcon.addFile(":/icons/app_16.png",  QSize(16, 16));
    appIcon.addFile(":/icons/app_24.png",  QSize(24, 24));
    appIcon.addFile(":/icons/app_32.png",  QSize(32, 32));
    appIcon.addFile(":/icons/app_256.png", QSize(256, 256)); // 大尺寸用于任务栏/Alt+Tab
    setWindowIcon(appIcon);
    resize(320, 260);
    auto* layout = new QFormLayout(this);

    m_nameCombo = new QComboBox();
    m_nameCombo->setEditable(true);
    layout->addRow("备注名:", m_nameCombo);

    m_ip = new QLineEdit("192.168.1.100");
    m_port = new QLineEdit("5900");
    m_pwd = new QLineEdit();
    m_pwd->setEchoMode(QLineEdit::Password);

    m_togglePwdBtn = new QPushButton("显示");
    m_togglePwdBtn->setFixedWidth(50);
    m_togglePwdBtn->setFocusPolicy(Qt::NoFocus);

    auto* pwdLayout = new QHBoxLayout();
    pwdLayout->addWidget(m_pwd);
    pwdLayout->addWidget(m_togglePwdBtn);

    layout->addRow("IP:", m_ip);
    layout->addRow("端口:", m_port);
    layout->addRow("密码:", pwdLayout);

    connect(m_togglePwdBtn, &QPushButton::clicked, this, [this]() {
        if (m_pwd->echoMode() == QLineEdit::Password) {
            m_pwd->setEchoMode(QLineEdit::Normal);
            m_togglePwdBtn->setText("隐藏");
        } else {
            m_pwd->setEchoMode(QLineEdit::Password);
            m_togglePwdBtn->setText("显示");
        }
    });

    m_remember = new QCheckBox("记住密码");
    layout->addRow(m_remember);

    auto* btn = new QPushButton("连接");
    layout->addRow(btn);
    m_status = new QLabel("");
    layout->addRow(m_status);

    connect(btn, &QPushButton::clicked, this, &LoginWindow::tryConnect);
    connect(m_nameCombo, &QComboBox::currentTextChanged, this, &LoginWindow::onProfileSelected);

    loadConfig();
}

void LoginWindow::onProfileSelected(const QString& name) {
    if (name.isEmpty()) return;
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QJsonArray profiles = doc.object()["profiles"].toArray();

    for (const auto& p : profiles) {
        QJsonObject po = p.toObject();
        if (po["name"].toString() == name) {
            m_ip->setText(po["ip"].toString());
            m_port->setText(po["port"].toString());
            bool rem = po["remember"].toBool();
            m_remember->setChecked(rem);
            if (rem && !po["password"].toString().isEmpty()) {
                std::string dec = proto::b64_decode(po["password"].toString().toStdString());
                m_pwd->setText(QString::fromStdString(dec));
            } else {
                m_pwd->clear();
            }
            break;
        }
    }
}

void LoginWindow::tryConnect() {
    QString ipStr = m_ip->text();
    int port = m_port->text().toUShort();
    QString pwdStr = m_pwd->text();
    QString name = m_nameCombo->currentText().trimmed();
    if (name.isEmpty()) name = ipStr;

    QFile f(configPath());
    if (f.open(QIODevice::ReadOnly)) {
        QJsonArray profiles = QJsonDocument::fromJson(f.readAll()).object()["profiles"].toArray();
        for (const auto& p : profiles) {
            QJsonObject po = p.toObject();
            if (po["name"].toString() == name && po["ip"].toString() != ipStr) {
                QMessageBox::warning(this, "错误", "备注名已存在，且 IP 不一致！\n请修改备注名。");
                return;
            }
        }
        f.close();
    }
    saveConfig(name, ipStr, m_port->text(), m_remember->isChecked(), pwdStr);

    m_status->setText("连接中...");
    std::thread([this, ipStr, port, pwdStr, name]{
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCK) {
            QMetaObject::invokeMethod(this, [this]{ m_status->setText("网络初始化失败"); }, Qt::QueuedConnection);
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        std::string ip_std = ipStr.toStdString();
        unsigned int a, b, c, d;
        if (sscanf(ip_std.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            addr.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
        } else {
            QMetaObject::invokeMethod(this, [this]{ m_status->setText("IP格式错误"); }, Qt::QueuedConnection);
            sock_close(s); return;
        }

        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
            QMetaObject::invokeMethod(this, [this]{ m_status->setText("连接失败"); }, Qt::QueuedConnection);
            sock_close(s); return;
        }

        std::string pwd = pwdStr.toStdString();
        uint32_t n = htonl((uint32_t)pwd.size());
        send_all(s, &n, 4);
        send_all(s, pwd.data(), pwd.size());
        uint8_t resp;
        if (!recv_exact(s, &resp, 1) || resp != 0x01) {
            QMetaObject::invokeMethod(this, [this]{ m_status->setText("密码错误"); }, Qt::QueuedConnection);
            sock_close(s); return;
        }

        uint8_t rb[8];
        if (!recv_exact(s, rb, 8)) { sock_close(s); return; }
        int rw = (int)((uint32_t(rb[0]) << 24) | (uint32_t(rb[1]) << 16) | (uint32_t(rb[2]) << 8) | uint32_t(rb[3]));
        int rh = (int)((uint32_t(rb[4]) << 24) | (uint32_t(rb[5]) << 16) | (uint32_t(rb[6]) << 8) | uint32_t(rb[7]));

        QMetaObject::invokeMethod(this, [this, s, rw, rh, name]{
            auto* w = new RemoteWindow(s, rw, rh, name);
            w->setAttribute(Qt::WA_DeleteOnClose);
            connect(w, &RemoteWindow::disconnected, this, &LoginWindow::show);
            w->show();
            this->hide();
        }, Qt::QueuedConnection);
    }).detach();
}

QString LoginWindow::configPath() {
    return QCoreApplication::applicationDirPath() + "/client_config.json";
}

void LoginWindow::loadConfig() {
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QJsonObject obj = doc.object();

    QString last_name = obj["last_name"].toString();
    QJsonArray profiles = obj["profiles"].toArray();

    for (const auto& p : profiles) {
        QJsonObject po = p.toObject();
        m_nameCombo->addItem(po["name"].toString());
    }

    if (!last_name.isEmpty()) {
        m_nameCombo->setCurrentText(last_name);
    } else if (m_nameCombo->count() > 0) {
        m_nameCombo->setCurrentIndex(0);
    }
    onProfileSelected(m_nameCombo->currentText());
}

void LoginWindow::saveConfig(const QString& name, const QString& ip, const QString& port,
                             bool remember, const QString& pwd) {
    QFile f(configPath());
    QJsonObject obj;
    if (f.open(QIODevice::ReadOnly)) {
        obj = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }

    QJsonArray profiles = obj["profiles"].toArray();
    std::string pwd_std = pwd.toStdString();
    QString b64pwd = remember ? QString::fromStdString(proto::b64_encode(pwd_std.c_str(), pwd_std.size())) : "";

    bool found = false;
    for (int i = 0; i < profiles.size(); ++i) {
        QJsonObject p = profiles[i].toObject();
        if (p["name"].toString() == name) {
            p["ip"] = ip; p["port"] = port; p["remember"] = remember; p["password"] = b64pwd;
            profiles[i] = p; found = true; break;
        }
    }
    if (!found) {
        QJsonObject p;
        p["name"] = name; p["ip"] = ip; p["port"] = port; p["remember"] = remember; p["password"] = b64pwd;
        profiles.append(p);
    }

    obj["profiles"] = profiles;
    obj["last_name"] = name;

    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson());
        f.close();
    }
}

int main(int argc, char** argv) {
    sock_global_init();
    QApplication app(argc, argv);
    QIcon appIcon;
    appIcon.addFile(":/icons/app_256.png", QSize(256, 256));
    app.setWindowIcon(appIcon); // 设置全局应用图标
    LoginWindow w;
    w.show();
    int r = app.exec();
    sock_global_cleanup();
    return r;
}

#include "main.moc"
