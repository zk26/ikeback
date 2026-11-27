/*
 * ClassroomShopPanel.h - teacher UI for the class shop and auctions
 *
 * Product management (upload, price, stock), auction control (start, close,
 * "highest bid wins") and the redemption queue. Every successful mutation
 * emits shopChanged() so the console broadcasts the new catalog to the
 * students at once. This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include "ClassroomShop.h"

class QLabel;
class QTableWidget;
class ClassroomLogStore;
class ClassroomRoster;

class ClassroomShopPanel : public QWidget
{
	Q_OBJECT
public:
	ClassroomShopPanel( const QString& dataDirectory, ClassroomRoster& roster,
						ClassroomLogStore& store, QWidget* parent = nullptr );

	void setClassName( const QString& className );
	void refresh();

Q_SIGNALS:
	//! Emitted after every successful book mutation (product, auction,
	//! redemption or shop open/close) so the console can push the catalog.
	void shopChanged();

private:
	void setupProductsCard();
	void setupAuctionCard();
	void setupRedemptionCard();

	void addProduct();
	void editProduct();
	void removeProduct();
	void toggleProductListed();
	void setProductStock();
	void productContextMenu( const QPoint& position );
	void adjustStock( int delta );
	void toggleShopOpen();

	void startAuction( const QString& productId = {} );
	void closeSelectedAuction();
	void removeSelectedAuction();

	void deliverRedemption();
	void rejectRedemption();

	QString currentProductId() const;
	QString currentAuctionId() const;
	QString currentRedemptionId() const;
	void appendShopEvent( const QString& detail, const QString& seat, const QString& student );

	QString m_dataDirectory;
	ClassroomRoster& m_roster;
	ClassroomLogStore& m_store;
	ClassroomShopBook m_book;
	QString m_className;

	QLabel* m_statusLabel = nullptr;
	QLabel* m_openLabel = nullptr;
	QTableWidget* m_productTable = nullptr;
	QTableWidget* m_auctionTable = nullptr;
	QTableWidget* m_redemptionTable = nullptr;
};
