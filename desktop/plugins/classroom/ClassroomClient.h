/*
 * ClassroomClient.h - non-topmost, non-closable student card, popups and sampler
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPushButton;
class QShowEvent;
class QTabWidget;
class QTextBrowser;
class QTimer;
class ClassroomAiChatWidget;
class ClassroomAiClient;
class ClassroomCheckInDialog;
class ClassroomScoreView;
class ClassroomShopView;
class ClassroomSilencePage;
class QTextBrowser;

class ClassroomClientWidget : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomClientWidget( QWidget* parent = nullptr );

	void setSettings( const QJsonObject& settings );
	void setRoster( const QJsonObject& roster );
	//! The worker never receives the bridge key, only this boolean.
	void setKeyConfigured( bool configured );
	void deliverAiReply( bool success, const QString& reply );
	void setIdentity( const QString& className, const QString& seat, const QString& student );
	void clearIdentity();
	void setLocked( bool locked );
	//! 「锁定全部」真正锁住键鼠，而不只是改标签。
	void setInputLocked( bool locked );
	void setScoreHtml( const QString& html );
	void setSilence( bool visible, const QString& text );
	void showCheckIn();
	//! Only the teacher's StopClient command may hide the card.
	void allowHide();
	void reactivate();
	//! Used by the ikeback tray icon context menu.
	void openAiAssistant();
	void requestProtectedQuit();
	void requestScore();
	//! Opens the class shop window and asks the teacher machine for the catalog.
	void openShop();
	//! Full catalog (products + auctions) from the teacher machine; balance is
	//! the student's own score total or a negative value when unknown.
	void applyShopState( const QJsonObject& catalog, int balance );
	//! Result of a redeem/bid. `catalog` is the state after the operation and
	//! may be empty.
	void shopReply( bool ok, const QString& message, int balance, const QJsonObject& catalog );
	void applyShopImage( const QString& productId, const QString& imageId, const QByteArray& image );

	QString className() const { return m_className; }
	QString seat() const { return m_seat; }
	QString student() const { return m_student; }
	bool loggedIn() const { return m_loggedIn; }

Q_SIGNALS:
	void eventOccurred( const QString& type, const QString& detail, const QString& app );
	void identityChanged( const QString& seat, const QString& student, bool loggedIn );
	void quitAuthorized();
	void scoreRequested();
	void callTeacherRequested();
	void checkInSubmitted( const QString& name );
	//! Forwarded by the plugin to the teacher machine, which owns the API key.
	void aiPromptRequested( const QString& prompt, const QVariantList& history );
	//! Shop: the card asks for the catalog; the plugin attaches the identity.
	void shopRequested();
	void shopRedeemRequested( const QString& productId );
	void shopBidRequested( const QString& auctionId, int amount );
	void shopImageRequested( const QString& productId, const QString& imageId );

protected:
	void closeEvent( QCloseEvent* event ) override;
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;

private:
	void handleLogin();
	void showAiChat();
	void requestQuit();
	void updateStatus();
	void updateVisibility();
	void placeOnScreen();
	bool rosterAccepts( const QString& seat, const QString& name ) const;
	ClassroomShopView* ensureShopView();

	QLabel* m_identityLabel = nullptr;
	QLabel* m_statusLabel = nullptr;
	QLabel* m_lockLabel = nullptr;
	QLabel* m_seatLabel = nullptr;
	QLabel* m_nameLabel = nullptr;
	QLineEdit* m_seatEdit = nullptr;
	QLineEdit* m_nameEdit = nullptr;
	QPushButton* m_loginButton = nullptr;
	QPushButton* m_aiButton = nullptr;
	QPushButton* m_scoreButton = nullptr;
	QPushButton* m_shopButton = nullptr;
	QPushButton* m_callButton = nullptr;
	QPushButton* m_quitButton = nullptr;

	ClassroomAiClient* m_aiClient = nullptr;
	ClassroomAiChatWidget* m_aiChat = nullptr;
	ClassroomScoreView* m_scoreView = nullptr;
	ClassroomShopView* m_shopView = nullptr;
	ClassroomSilencePage* m_silencePage = nullptr;
	ClassroomSilencePage* m_lockOverlay = nullptr;
	ClassroomCheckInDialog* m_checkIn = nullptr;

	bool m_keyConfigured = false;
	QString m_className;
	QString m_seat;
	QString m_student;
	QJsonObject m_roster;
	//! SHA-256 hex of the super-admin key (never the key itself).
	QString m_adminKeyHash;

	bool m_loggedIn = false;
	bool m_locked = false;
	bool m_allowSelfLogin = true;
	bool m_aiEnabled = true;
	bool m_exitProtection = true;
	QString m_exitAdminUsername;
	QPoint m_dragPosition;
	//! The card must stay on screen for as long as the ikeback service runs.
	QTimer* m_guardTimer = nullptr;
	bool m_hideAllowed = false;
	//! Only one protected-exit dialog at a time - repeated close attempts must
	//! not stack up modal dialogs on the student desktop.
	bool m_quitDialogOpen = false;
};


class ClassroomAiChatWidget : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomAiChatWidget( ClassroomAiClient* client, QWidget* parent = nullptr );

	void setIdentityLabel( const QString& label );

Q_SIGNALS:
	void eventOccurred( const QString& type, const QString& detail, const QString& app );

protected:
	void closeEvent( QCloseEvent* event ) override;
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;

private:
	void sendMessage();
	void appendLine( const QString& html );

	ClassroomAiClient* m_aiClient = nullptr;
	QTextBrowser* m_view = nullptr;
	QLineEdit* m_input = nullptr;
	QPushButton* m_sendButton = nullptr;
	QVariantList m_history;
	QString m_lastPrompt;
	QPoint m_dragPosition;
};


class ClassroomScoreView : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomScoreView( QWidget* parent = nullptr );
	void setHtml( const QString& html );

private:
	QTextBrowser* m_view = nullptr;
};


//! Class shop window on the student card: a catalog of products (redeem with
//! classroom score) plus the auction area (highest bid wins). The teacher
//! machine owns all data; the window only renders catalog JSON and sends
//! redeem/bid requests through the plugin.
class ClassroomShopView : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomShopView( QWidget* parent = nullptr );

	//! Replaces the whole catalog. balance < 0 leaves the balance display
	//! unchanged (broadcast states carry no per-student balance).
	void applyCatalog( const QJsonObject& catalog, int balance );
	void applyReply( bool ok, const QString& message, int balance, const QJsonObject& catalog );
	void applyImage( const QString& productId, const QString& imageId, const QByteArray& image );
	//! Who is signed in: needed to tell "you were outbid" from "you won".
	void setIdentity( const QString& seat, const QString& student );
	void setSignedIn( bool signedIn );

Q_SIGNALS:
	void refreshRequested();
	void redeemRequested( const QString& productId );
	void bidRequested( const QString& auctionId, int amount );
	void imageRequested( const QString& productId, const QString& imageId );
	//! Something worth showing even while the shop window is closed.
	void noticeRaised( const QString& text );

protected:
	void showEvent( QShowEvent* event ) override;
	void closeEvent( QCloseEvent* event ) override;
	void mousePressEvent( QMouseEvent* event ) override;
	void mouseMoveEvent( QMouseEvent* event ) override;

private:
	void rebuild();
	void requestMissingImages();
	QWidget* buildProductCard( const QJsonObject& product );
	QWidget* buildAuctionCard( const QJsonObject& auction );
	static QImage decodeImage( const QByteArray& image );

	QLabel* m_balanceLabel = nullptr;
	QLabel* m_statusLabel = nullptr;
	QTabWidget* m_tabs = nullptr;
	QWidget* m_productsArea = nullptr;
	QWidget* m_auctionsArea = nullptr;
	QJsonObject m_catalog;
	int m_balance = -1;
	QString m_seat;
	QString m_student;
	bool m_signedIn = false;
	//! auction id -> (was open, leading seat) of the previous catalog.
	QHash<QString, QPair<bool, QString>> m_auctionStates;
	QHash<QString, QPair<QString, QImage>> m_images;
	QHash<QString, QString> m_requestedImages;
	QHash<QString, QString> m_failedImages;
	QTimer* m_actionTimer = nullptr;
	bool m_actionPending = false;
	QPoint m_dragPosition;
};


class ClassroomSilencePage : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomSilencePage( QWidget* parent = nullptr );
	~ClassroomSilencePage() override;
	void setText( const QString& text );
	void setInputLocked( bool locked );

private:
	QLabel* m_label = nullptr;
	bool m_inputLocked = false;
};


//! Attendance prompt shown on every student desktop when the teacher starts a check-in.
class ClassroomCheckInDialog : public QWidget
{
	Q_OBJECT
public:
	explicit ClassroomCheckInDialog( QWidget* parent = nullptr );

	void prepare( const QString& className, const QString& seat );

Q_SIGNALS:
	void submitted( const QString& name );

private:
	void submit();

	QLabel* m_info = nullptr;
	QLineEdit* m_nameEdit = nullptr;
};


class ClassroomMonitor : public QObject
{
	Q_OBJECT
public:
	explicit ClassroomMonitor( QObject* parent = nullptr );

	void setSettings( const QJsonObject& settings );
	void start();
	void stop();

Q_SIGNALS:
	void activity( const QString& app, const QString& title, bool browser );
	//! Emitted when a fuzzy (case-insensitive substring) app / website rule matches.
	void blockedActivity( const QString& app, const QString& title, const QString& rule, bool browser );

private:
	void sample();
	static QPair<QString, QString> foregroundWindowInfo();

	QTimer* m_timer = nullptr;
	QString m_lastSample;
	QStringList m_blockedApps;
	QStringList m_blockedWebsites;
	bool m_enabled = true;
	bool m_captureWindowTitle = true;
	bool m_captureProcess = true;
};
