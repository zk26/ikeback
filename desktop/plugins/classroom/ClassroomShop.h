/*
 * ClassroomShop.h - per-class shop and auction book, kept on the teacher machine
 *
 * Student balances ARE their classroom scores: a redemption or a won auction
 * is simply a negative score entry in the class score book, so the score
 * panel, the seat grid, the term reports and the phone all stay consistent
 * without a second ledger. Student machines never write shop data; the
 * teacher machine validates every redeem / bid against the live score book.
 *
 * Data lives in <dataDirectory>/shop/<class>.json and is atomically replaced
 * after every change. Product pictures are stored in the same file so the
 * catalog metadata and pictures cannot diverge after a failed save.
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

class QImage;

//! One product of the class shop.
struct ClassroomShopProduct
{
	QString id;
	QString name;
	QString description;
	int price = 0;
	int stock = 0;
	bool listed = true;
	//! PNG bytes of the uploaded picture, empty when there is none. It is sent
	//! separately on demand, so normal catalog broadcasts stay small.
	QByteArray image;
	QString imageId;

	QJsonObject toJson( bool withImage = true ) const;
	static ClassroomShopProduct fromJson( const QJsonObject& object );
};

//! One accepted bid. Bids are stored in ascending order, so the last bid is
//! always the winning one ("价高者得").
struct ClassroomShopBid
{
	QString seat;
	QString student;
	int amount = 0;
	QDateTime time;

	QJsonObject toStorage() const;
	static ClassroomShopBid fromStorage( const QJsonObject& object );
	QJsonObject toJson() const;
};

struct ClassroomShopAuction
{
	QString id;
	QString name;
	int startPrice = 1;
	int minIncrement = 1;
	bool open = true;
	QVector<ClassroomShopBid> bids;

	int currentBid() const;
	QString bidderSeat() const;
	QString bidderStudent() const;
	//! Lowest amount the next bid may have.
	int minNextBid() const;

	QJsonObject toStorage() const;
	static ClassroomShopAuction fromStorage( const QJsonObject& object );
	//! Student view: state, leader and the most recent bids.
	QJsonObject toJson() const;
};

//! Outcome of closeAuction().
struct ClassroomShopAuctionResult
{
	//! False when the auction does not exist, was already closed, or could not
	//! be booked (see error).
	bool found = false;
	bool sold = false;
	int price = 0;
	QString auctionId;
	QString name;
	QString winnerSeat;
	QString winnerStudent;
	//! Non-empty when the win could not be saved; the auction stays open.
	QString error;
};

//! One item the teacher has to hand out: a shop purchase or an auction win.
struct ClassroomShopRedemption
{
	enum Status
	{
		Pending = 0,
		Delivered = 1,
		Rejected = 2
	};

	enum Kind
	{
		Purchase = 0,
		AuctionWin = 1
	};

	QString id;
	QString productId;
	QString productName;
	QString seat;
	QString student;
	int price = 0;
	int status = Pending;
	int kind = Purchase;
	QDateTime timestamp;

	QJsonObject toStorage() const;
	static ClassroomShopRedemption fromStorage( const QJsonObject& object );
};

class ClassroomShopBook
{
public:
	//! Validation limits shared by the UI, the plugin handlers and the self test.
	enum Limits
	{
		MaxProducts = 100,
		MaxOpenAuctions = 20,
		MaxStoredAuctions = 200,
		MaxPrice = 100000,
		MaxStock = 9999,
		MaxIncrement = 1000,
		MaxBid = 1000000,
		MaxImageBytes = 40 * 1024,
		MaxStoredBids = 500,
		MaxListedBids = 30,
		MaxRedemptions = 300,
		MaxNameLength = 60,
		MaxDescriptionLength = 300
	};

	//! Lesson topic of the score entries the shop writes. Kept as a plain
	//! Chinese constant because it is persisted data, not UI text.
	static QString lessonTopic() { return QStringLiteral("班级商场"); }
	static constexpr int ProtectedScore = 80;
	static constexpr int spendableScore( int balance ) { return qMax( 0, balance - ProtectedScore ); }

	explicit ClassroomShopBook( const QString& dataDirectory );

	static QString directoryPath( const QString& dataDirectory );
	QString filePath() const;

	//! Reads the book of `className`. An empty class name or a missing file
	//! gives an empty book; false is returned for unreadable or invalid data.
	bool load( const QString& className );

	const QString& className() const { return m_className; }
	bool isOpen() const { return m_open; }
	bool isLoaded() const { return m_loaded; }
	//! "关店": while closed nobody can redeem or bid; the catalog stays visible.
	bool setOpen( bool open );
	qint64 revision() const { return m_revision; }

	const QVector<ClassroomShopProduct>& products() const { return m_products; }
	const QVector<ClassroomShopAuction>& auctions() const { return m_auctions; }
	const QVector<ClassroomShopRedemption>& redemptions() const { return m_redemptions; }
	const ClassroomShopProduct* product( const QString& productId ) const;
	QByteArray productImage( const QString& productId ) const;
	const ClassroomShopAuction* auction( const QString& auctionId ) const;
	const ClassroomShopRedemption* redemption( const QString& redemptionId ) const;

	// ------------------------------------------------------------- catalog
	//! Appends the product and assigns its id. Returns false when the catalog
	//! is full, the name is empty or the book could not be saved.
	bool addProduct( ClassroomShopProduct* product );
	bool updateProduct( const ClassroomShopProduct& product );
	bool removeProduct( const QString& productId );
	bool adjustStock( const QString& productId, int delta );
	bool setProductListed( const QString& productId, bool listed );

	// ------------------------------------------------------------ auctions
	//! Returns the new auction id, or an empty string when nothing can be
	//! started (empty name, too many open auctions, save failure).
	QString startAuction( const QString& name, int startPrice, int minIncrement );
	//! Ends the auction. With at least one bid the last (highest) one wins:
	//! its amount is booked as a negative score entry and a redemption is
	//! queued. Without bids the auction simply closes unsold.
	ClassroomShopAuctionResult closeAuction( const QString& auctionId );
	//! Deletes the record. Deleting an open auction cancels it: nobody pays.
	bool removeAuction( const QString& auctionId );

	// -------------------------------------------------- student requests
	//! Redeems one unit of a product for (seat, student). `balance` is the
	//! student's current score, which the caller reads from the score book.
	//! Returns an empty string on success, otherwise a Chinese reason that is
	//! shown to the student as is. Books the score entry itself.
	QString redeem( const QString& productId, const QString& seat, const QString& student, int balance );
	//! Places a bid. No points change until the auction is closed.
	QString placeBid( const QString& auctionId, int amount, const QString& seat,
					  const QString& student, int balance );

	// ------------------------------------------------------- delivery queue
	//! Marks a redemption delivered / rejected. Rejecting refunds the price as
	//! a positive score entry and, for a shop purchase, puts the unit back
	//! into stock. Rejecting an already rejected entry does nothing.
	bool setRedemptionStatus( const QString& redemptionId, int status );

	// --------------------------------------------------------------- scores
	int balance( const QString& seat ) const;
	//! Appends a shop score entry to the class score book (negative = spent,
	//! positive = refunded). A zero delta is a no-op and returns true.
	bool bookScore( const QString& seat, const QString& student, int delta, const QString& label );

	// -------------------------------------------------------------- payloads
	//! Everything a student may see: shop state, products and auctions.
	//! Pictures are omitted by default and fetched with productImage().
	QJsonObject catalogJson( bool withImages = false ) const;

	//! Scales and re-encodes an uploaded picture until it fits the catalog
	//! budget. PNG only: it is built into QtGui, so no image format plugin is
	//! needed on either side. Returns an empty byte array for unusable input.
	static QByteArray catalogImage( const QImage& image );

private:
	bool save();
	bool makeRedemptionCapacity();

	QString m_basePath;
	QString m_className;
	bool m_loaded = false;
	bool m_open = true;
	qint64 m_revision = 0;
	QVector<ClassroomShopProduct> m_products;
	QVector<ClassroomShopAuction> m_auctions;
	QVector<ClassroomShopRedemption> m_redemptions;
};
