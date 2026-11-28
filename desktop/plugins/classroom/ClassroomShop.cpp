/*
 * ClassroomShop.cpp - per-class shop and auction book on the teacher machine
 *
 * Score-changing operations first persist a pending journal with a fixed
 * score-entry id, then write that entry, then atomically commit the shop
 * mutation and clear the journal. Recovery removes an uncommitted charge or
 * completes a committed refund, so retries never apply the delta twice.
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <QBuffer>
#include <algorithm>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <QVariant>

#include "ClassroomData.h"
#include "ClassroomScore.h"
#include "ClassroomShop.h"

namespace
{

QString newShopId()
{
	return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

//! Reads a whole file. `ok` is false only when the file exists but cannot be
//! read; a missing file is an empty byte array with ok == true.
QByteArray readShopFile( const QString& fileName, bool* ok )
{
	*ok = true;
	if( QFileInfo::exists( fileName ) == false ) { return {}; }
	QFile file( fileName );
	if( file.open( QFile::ReadOnly ) == false ) { *ok = false; return {}; }
	return file.readAll();
}


bool writeShopFile( const QString& fileName, const QByteArray& bytes )
{
	QSaveFile file( fileName );
	if( file.open( QIODevice::WriteOnly ) == false ) { return false; }
	if( file.write( bytes ) != bytes.size() ) { return false; }
	return file.commit();
}


template<typename T>
const T* shopItemById( const QVector<T>& items, const QString& id )
{
	for( const auto& item : items )
	{
		if( item.id == id ) { return &item; }
	}
	return nullptr;
}


template<typename T>
int shopIndexById( const QVector<T>& items, const QString& id )
{
	for( int index = 0; index < items.size(); ++index )
	{
		if( items.at( index ).id == id ) { return index; }
	}
	return -1;
}


QJsonArray storageArray( const QVector<ClassroomShopBid>& bids )
{
	QJsonArray array;
	for( const auto& bid : bids ) { array.append( bid.toStorage() ); }
	return array;
}


QJsonArray storageArray( const QVector<ClassroomShopAuction>& auctions )
{
	QJsonArray array;
	for( const auto& auction : auctions ) { array.append( auction.toStorage() ); }
	return array;
}


QJsonArray storageArray( const QVector<ClassroomShopRedemption>& redemptions )
{
	QJsonArray array;
	for( const auto& redemption : redemptions ) { array.append( redemption.toStorage() ); }
	return array;
}


QDateTime shopTimeStamp( const QJsonObject& object, const QString& key )
{
	const auto stamp = QDateTime::fromString( object.value( key ).toString(), Qt::ISODateWithMs );
	return stamp.isValid() ? stamp : QDateTime::currentDateTime();
}


QString boundedName( const QString& name )
{
	return name.trimmed().left( ClassroomShopBook::MaxNameLength );
}


constexpr int ShopJournalVersion = 1;
constexpr int MaxCompletedShopOperations = 128;


bool validPendingOperation( const QJsonObject& operation )
{
	const auto kind = operation.value( QStringLiteral("kind") ).toString();
	const auto redemption = operation.value( QStringLiteral("redemption") ).toObject();
	const int delta = operation.value( QStringLiteral("delta") ).toInt();
	if( operation.value( QStringLiteral("version") ).toInt() != ShopJournalVersion ||
		operation.value( QStringLiteral("id") ).toString().isEmpty() ||
		operation.value( QStringLiteral("scoreEntryId") ).toString().isEmpty() ||
		operation.value( QStringLiteral("seat") ).toString().isEmpty() ||
		operation.value( QStringLiteral("student") ).toString().isEmpty() ||
		redemption.value( QStringLiteral("id") ).toString().isEmpty() ||
		redemption.value( QStringLiteral("price") ).toInt() != qAbs( delta ) )
	{
		return false;
	}
	if( kind == QStringLiteral("purchase") || kind == QStringLiteral("auction") )
	{
		return delta <= 0 &&
			( kind != QStringLiteral("auction") ||
			  operation.value( QStringLiteral("auctionId") ).toString().isEmpty() == false );
	}
	if( kind == QStringLiteral("reject") )
	{
		return delta >= 0 &&
			operation.value( QStringLiteral("redemptionId") ).toString() ==
				redemption.value( QStringLiteral("id") ).toString();
	}
	return false;
}


QString redeemFingerprint( const QString& productId, const QString& seat, const QString& student )
{
	return QString::fromLatin1( QJsonDocument( QJsonArray{ productId, seat, student } ).
		toJson( QJsonDocument::Compact ) );
}


bool tryLockShopFile( QLockFile& lock )
{
	if( QDir().mkpath( QFileInfo( lock.fileName() ).absolutePath() ) == false ) { return false; }
	return lock.tryLock( 1000 );
}


bool tryLockScoreFile( QLockFile& lock )
{
	if( QDir().mkpath( QFileInfo( lock.fileName() ).absolutePath() ) == false ) { return false; }
	return lock.tryLock( 1000 );
}


} // namespace


// ------------------------------------------------------------------ product

QJsonObject ClassroomShopProduct::toJson( bool withImage ) const
{
	QJsonObject object{
		{ QStringLiteral("id"), id },
		{ QStringLiteral("name"), name },
		{ QStringLiteral("description"), description },
		{ QStringLiteral("price"), price },
		{ QStringLiteral("stock"), stock },
		{ QStringLiteral("listed"), listed },
		{ QStringLiteral("hasImage"), image.isEmpty() == false },
		{ QStringLiteral("imageId"), imageId } };
	if( withImage && image.isEmpty() == false )
	{
		object.insert( QStringLiteral("image"), QString::fromLatin1( image.toBase64() ) );
	}
	return object;
}


ClassroomShopProduct ClassroomShopProduct::fromJson( const QJsonObject& object )
{
	ClassroomShopProduct product;
	product.id = object.value( QStringLiteral("id") ).toString();
	product.name = boundedName( object.value( QStringLiteral("name") ).toString() );
	product.description = object.value( QStringLiteral("description") ).toString().trimmed().
		left( ClassroomShopBook::MaxDescriptionLength );
	product.price = qBound( 0, object.value( QStringLiteral("price") ).toInt(),
							 static_cast<int>( ClassroomShopBook::MaxPrice ) );
	product.stock = qBound( 0, object.value( QStringLiteral("stock") ).toInt(),
							static_cast<int>( ClassroomShopBook::MaxStock ) );
	product.listed = object.value( QStringLiteral("listed") ).toBool( true );
	product.image = QByteArray::fromBase64( object.value( QStringLiteral("image") ).toString().toLatin1() );
	if( product.image.size() > ClassroomShopBook::MaxImageBytes ) { product.image.clear(); }
	product.imageId = object.value( QStringLiteral("imageId") ).toString();
	return product;
}


// --------------------------------------------------------------------- bids

QJsonObject ClassroomShopBid::toStorage() const
{
	return { { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("amount"), amount },
			 { QStringLiteral("ts"), time.toString( Qt::ISODateWithMs ) } };
}


ClassroomShopBid ClassroomShopBid::fromStorage( const QJsonObject& object )
{
	ClassroomShopBid bid;
	bid.seat = object.value( QStringLiteral("seat") ).toString().trimmed().left( 16 );
	bid.student = object.value( QStringLiteral("student") ).toString().trimmed().left( 32 );
	bid.amount = qBound( 0, object.value( QStringLiteral("amount") ).toInt(),
						 static_cast<int>( ClassroomShopBook::MaxBid ) );
	bid.time = shopTimeStamp( object, QStringLiteral("ts") );
	return bid;
}


QJsonObject ClassroomShopBid::toJson() const
{
	return { { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("amount"), amount },
			 { QStringLiteral("time"), time.toString( QStringLiteral("HH:mm:ss") ) } };
}


// ----------------------------------------------------------------- auctions

int ClassroomShopAuction::currentBid() const
{
	return bids.isEmpty() ? 0 : bids.last().amount;
}


QString ClassroomShopAuction::bidderSeat() const
{
	return bids.isEmpty() ? QString{} : bids.last().seat;
}


QString ClassroomShopAuction::bidderStudent() const
{
	return bids.isEmpty() ? QString{} : bids.last().student;
}


int ClassroomShopAuction::minNextBid() const
{
	return bids.isEmpty() ? startPrice : currentBid() + minIncrement;
}


QJsonObject ClassroomShopAuction::toStorage() const
{
	return { { QStringLiteral("id"), id },
			 { QStringLiteral("name"), name },
			 { QStringLiteral("startPrice"), startPrice },
			 { QStringLiteral("minIncrement"), minIncrement },
			 { QStringLiteral("open"), open },
			 { QStringLiteral("bids"), storageArray( bids ) } };
}


ClassroomShopAuction ClassroomShopAuction::fromStorage( const QJsonObject& object )
{
	ClassroomShopAuction auction;
	auction.id = object.value( QStringLiteral("id") ).toString();
	auction.name = boundedName( object.value( QStringLiteral("name") ).toString() );
	auction.startPrice = qBound( 1, object.value( QStringLiteral("startPrice") ).toInt( 1 ),
								 static_cast<int>( ClassroomShopBook::MaxPrice ) );
	auction.minIncrement = qBound( 1, object.value( QStringLiteral("minIncrement") ).toInt( 1 ),
								   static_cast<int>( ClassroomShopBook::MaxIncrement ) );
	auction.open = object.value( QStringLiteral("open") ).toBool( true );
	for( const auto& value : object.value( QStringLiteral("bids") ).toArray() )
	{
		if( static_cast<int>( auction.bids.size() ) >= ClassroomShopBook::MaxStoredBids ) { break; }
		auction.bids.append( ClassroomShopBid::fromStorage( value.toObject() ) );
	}
	std::stable_sort( auction.bids.begin(), auction.bids.end(),
		[]( const ClassroomShopBid& a, const ClassroomShopBid& b ) { return a.amount < b.amount; } );
	return auction;
}


QJsonObject ClassroomShopAuction::toJson() const
{
	const int count = static_cast<int>( bids.size() );
	const int listed = static_cast<int>( ClassroomShopBook::MaxListedBids );
	QJsonArray recent;
	for( int index = qMax( 0, count - listed ); index < count; ++index )
	{
		recent.append( bids.at( index ).toJson() );
	}

	QJsonObject object{
		{ QStringLiteral("id"), id },
		{ QStringLiteral("name"), name },
		{ QStringLiteral("startPrice"), startPrice },
		{ QStringLiteral("minIncrement"), minIncrement },
		{ QStringLiteral("open"), open },
		{ QStringLiteral("currentBid"), currentBid() },
		{ QStringLiteral("minNextBid"), minNextBid() },
		{ QStringLiteral("bidderSeat"), bidderSeat() },
		{ QStringLiteral("bidderStudent"), bidderStudent() },
		{ QStringLiteral("bids"), recent } };
	if( open == false )
	{
		// Closed without a single bid: currentBid() is 0 and nobody wins.
		object.insert( QStringLiteral("finalPrice"), currentBid() );
		object.insert( QStringLiteral("winnerSeat"), bidderSeat() );
		object.insert( QStringLiteral("winnerStudent"), bidderStudent() );
	}
	return object;
}


// -------------------------------------------------------------- redemptions

QJsonObject ClassroomShopRedemption::toStorage() const
{
	return { { QStringLiteral("id"), id },
			 { QStringLiteral("productId"), productId },
			 { QStringLiteral("productName"), productName },
			 { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("price"), price },
			 { QStringLiteral("status"), status },
			 { QStringLiteral("kind"), kind },
			 { QStringLiteral("ts"), timestamp.toString( Qt::ISODateWithMs ) } };
}


ClassroomShopRedemption ClassroomShopRedemption::fromStorage( const QJsonObject& object )
{
	ClassroomShopRedemption redemption;
	redemption.id = object.value( QStringLiteral("id") ).toString();
	redemption.productId = object.value( QStringLiteral("productId") ).toString();
	redemption.productName = object.value( QStringLiteral("productName") ).toString();
	redemption.seat = object.value( QStringLiteral("seat") ).toString();
	redemption.student = object.value( QStringLiteral("student") ).toString();
	redemption.price = object.value( QStringLiteral("price") ).toInt();
	redemption.status = qBound( 0, object.value( QStringLiteral("status") ).toInt( Pending ),
								static_cast<int>( Rejected ) );
	redemption.kind = qBound( 0, object.value( QStringLiteral("kind") ).toInt( Purchase ),
							  static_cast<int>( AuctionWin ) );
	redemption.timestamp = shopTimeStamp( object, QStringLiteral("ts") );
	return redemption;
}


// --------------------------------------------------------------------- book

ClassroomShopBook::ClassroomShopBook( const QString& dataDirectory ) :
	m_basePath( dataDirectory )
{
}


QString ClassroomShopBook::directoryPath( const QString& dataDirectory )
{
	return QDir( dataDirectory ).filePath( QStringLiteral("shop") );
}


QString ClassroomShopBook::filePath() const
{
	return QDir( directoryPath( m_basePath ) ).filePath(
		ClassroomLogStore::storageKey( m_className ) + QStringLiteral(".json") );
}


//! Old installations stored the book under the lossy sanitize() name.
void ClassroomShopBook::migrateLegacyFile() const
{
	if( m_className.isEmpty() ) { return; }
	ClassroomLogStore::migrateStorageKey( directoryPath( m_basePath ),
					   ClassroomLogStore::sanitize( m_className ) + QStringLiteral(".json"),
					   ClassroomLogStore::storageKey( m_className ) + QStringLiteral(".json") );
}


bool ClassroomShopBook::load( const QString& className )
{
	const auto previousClassName = m_className;
	const auto previousLoaded = m_loaded;
	const auto previousPersisted = m_persisted;
	const auto previousOpen = m_open;
	const auto previousRevision = m_revision;
	const auto previousProducts = m_products;
	const auto previousAuctions = m_auctions;
	const auto previousRedemptions = m_redemptions;
	const auto previousPendingOperation = m_pendingOperation;
	const auto previousCompletedOperations = m_completedOperations;
	const auto previousError = m_lastError;
	m_className = className;
	m_loaded = false;
	m_persisted = false;
	m_open = true;
	m_revision = 0;
	m_products.clear();
	m_auctions.clear();
	m_redemptions.clear();
	m_pendingOperation = QJsonObject{};
	m_completedOperations = QJsonArray{};
	m_lastError.clear();
	const auto fail = [&]() {
		m_className = previousClassName;
		m_loaded = previousLoaded;
		m_persisted = previousPersisted;
		m_open = previousOpen;
		m_revision = previousRevision;
		m_products = previousProducts;
		m_auctions = previousAuctions;
		m_redemptions = previousRedemptions;
		m_pendingOperation = previousPendingOperation;
		m_completedOperations = previousCompletedOperations;
		m_lastError = previousError;
		return false;
	};
	if( m_className.isEmpty() ) { m_loaded = true; return true; }

	const auto modernFileName = filePath();
	const auto legacyFileName = QDir( directoryPath( m_basePath ) ).filePath(
		ClassroomLogStore::sanitize( m_className ) + QStringLiteral(".json") );
	const auto fileName = QFile::exists( modernFileName ) == false && QFile::exists( legacyFileName ) ?
							  legacyFileName : modernFileName;
	bool ok = true;
	const auto data = readShopFile( fileName, &ok );
	if( ok == false ) { return fail(); }
	if( data.isEmpty() && QFileInfo::exists( fileName ) == false &&
		className.isEmpty() == false && className == previousClassName && previousPersisted ) { return fail(); }
	if( data.isEmpty() && QFileInfo::exists( fileName ) ) { return fail(); }
	if( data.isEmpty() == false )
	{
		const auto document = QJsonDocument::fromJson( data );
		if( document.isObject() == false ) { return fail(); }
		const auto object = document.object();
		const auto productsValue = object.value( QStringLiteral("products") );
		const auto auctionsValue = object.value( QStringLiteral("auctions") );
		const auto redemptionsValue = object.value( QStringLiteral("redemptions") );
		const auto completedValue = object.value( QStringLiteral("completedOperations") );
		if( productsValue.isArray() == false || auctionsValue.isArray() == false ||
			redemptionsValue.isArray() == false ||
			( completedValue.isUndefined() == false && completedValue.isArray() == false ) )
		{
			return fail();
		}
		m_open = object.value( QStringLiteral("open") ).toBool( true );
		m_revision = qMax<qint64>( 0, object.value( QStringLiteral("revision") ).toVariant().toLongLong() );
		for( const auto& value : productsValue.toArray() )
		{
			if( value.isObject() == false || static_cast<int>( m_products.size() ) >= MaxProducts ) { return fail(); }
			m_products.append( ClassroomShopProduct::fromJson( value.toObject() ) );
		}
		for( const auto& value : auctionsValue.toArray() )
		{
			if( value.isObject() == false ||
				static_cast<int>( m_auctions.size() ) >= ClassroomShopBook::MaxStoredAuctions ) { return fail(); }
			m_auctions.append( ClassroomShopAuction::fromStorage( value.toObject() ) );
		}
		for( const auto& value : redemptionsValue.toArray() )
		{
			if( value.isObject() == false || static_cast<int>( m_redemptions.size() ) >= MaxRedemptions ) { return fail(); }
			m_redemptions.append( ClassroomShopRedemption::fromStorage( value.toObject() ) );
		}
		const auto pendingValue = object.value( QStringLiteral("pendingOperation") );
		if( pendingValue.isObject() )
		{
			if( pendingValue.toObject().isEmpty() == false )
			{
				m_pendingOperation = pendingValue.toObject();
				if( validPendingOperation( m_pendingOperation ) == false ) { return fail(); }
			}
		}
		else if( pendingValue.isNull() == false && pendingValue.isUndefined() == false )
		{
			return fail();
		}
		for( const auto& value : completedValue.toArray() )
		{
			if( value.isObject() == false ) { return fail(); }
			const auto completed = value.toObject();
			if( completed.value( QStringLiteral("id") ).toString().isEmpty() == false &&
				completed.value( QStringLiteral("fingerprint") ).toString().isEmpty() == false )
			{
				m_completedOperations.append( completed );
			}
		}
		while( m_completedOperations.size() > MaxCompletedShopOperations )
		{
			m_completedOperations.removeFirst();
		}
	}
	if( fileName != filePath() )
	{
		migrateLegacyFile();
	}
	m_loaded = true;
	m_persisted = data.isEmpty() == false;
	return true;
}


bool ClassroomShopBook::save()
{
	if( m_className.isEmpty() || m_loaded == false ) { return false; }
	if( QDir().mkpath( directoryPath( m_basePath ) ) == false ) { return false; }

	QJsonArray products;
	for( const auto& product : m_products )
	{
		products.append( product.toJson( true ) );
	}

	const QJsonObject object{
		{ QStringLiteral("revision"), m_revision },
		{ QStringLiteral("open"), m_open },
		{ QStringLiteral("products"), products },
		{ QStringLiteral("auctions"), storageArray( m_auctions ) },
		{ QStringLiteral("redemptions"), storageArray( m_redemptions ) },
		{ QStringLiteral("pendingOperation"), m_pendingOperation },
		{ QStringLiteral("completedOperations"), m_completedOperations } };
	if( writeShopFile( filePath(), QJsonDocument( object ).toJson( QJsonDocument::Indented ) ) == false ) { return false; }
	m_persisted = true;
	return true;
}


bool ClassroomShopBook::preparePendingOperation( const QJsonObject& operation )
{
	if( m_loaded == false || m_pendingOperation.isEmpty() == false ||
		validPendingOperation( operation ) == false )
	{
		return false;
	}
	m_pendingOperation = operation;
	++m_revision;
	if( save() ) { return true; }

	m_pendingOperation = QJsonObject{};
	--m_revision;
	const auto error = QStringLiteral( "无法保存商城操作日志。" );
	// QSaveFile is atomic, but reloading also reconciles the unlikely case of
	// an ambiguous commit result before another operation is allowed.
	load( m_className );
	m_lastError = error;
	return false;
}


bool ClassroomShopBook::appendScoreEntry( const ClassroomScoreEntry& entry )
{
	if( entry.delta == 0 ) { return true; }
	ClassroomScoreBook book( m_basePath );
	QLockFile lock( book.filePath( m_className ) + QStringLiteral( ".lock" ) );
	if( tryLockScoreFile( lock ) == false || book.load( m_className ) == false ) { return false; }
	if( entry.id.isEmpty() == false )
	{
		for( const auto& existing : book.entries() )
		{
			if( existing.id == entry.id ) { return true; }
		}
	}

	auto copy = entry;
	if( copy.lessonId.isEmpty() )
	{
		copy.lessonId = book.ensureLesson( QDate::currentDate(), lessonTopic() );
		if( copy.lessonId.isEmpty() ) { return false; }
	}
	if( book.addEntry( copy ) ) { return true; }
	// addEntry() keeps the unsaved item in memory when save() fails.
	book.load( m_className );
	return false;
}


bool ClassroomShopBook::scoreEntryExists( const QString& entryId, bool* exists )
{
	*exists = false;
	ClassroomScoreBook book( m_basePath );
	QLockFile lock( book.filePath( m_className ) + QStringLiteral( ".lock" ) );
	if( tryLockScoreFile( lock ) == false || book.load( m_className ) == false ) { return false; }
	for( const auto& entry : book.entries() )
	{
		if( entry.id == entryId ) { *exists = true; return true; }
	}
	return true;
}


bool ClassroomShopBook::removeScoreEntryExactly( const QString& entryId )
{
	ClassroomScoreBook book( m_basePath );
	QLockFile lock( book.filePath( m_className ) + QStringLiteral( ".lock" ) );
	if( tryLockScoreFile( lock ) == false || book.load( m_className ) == false ) { return false; }
	bool found = false;
	for( const auto& entry : book.entries() )
	{
		if( entry.id == entryId ) { found = true; break; }
	}
	if( found == false ) { return true; }
	if( book.removeEntry( entryId ) ) { return true; }
	book.load( m_className );
	for( const auto& entry : book.entries() )
	{
		if( entry.id == entryId ) { return false; }
	}
	return true;
}


bool ClassroomShopBook::recoverPendingOperations()
{
	if( m_className.isEmpty() ) { return true; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	return recoverPendingOperationsLocked();
}


bool ClassroomShopBook::recoverPendingOperationsLocked()
{
	if( m_pendingOperation.isEmpty() ) { m_lastError.clear(); return true; }
	if( m_recovering ) { return false; }

	m_recovering = true;
	const auto operation = m_pendingOperation;
	const auto kind = operation.value( QStringLiteral("kind") ).toString();
	const auto scoreEntryId = operation.value( QStringLiteral("scoreEntryId") ).toString();
	bool recovered = false;

	if( kind == QStringLiteral("purchase") || kind == QStringLiteral("auction") )
	{
		// The shop commit clears the journal atomically. If it is still here,
		// the charge is not committed and its fixed id can be removed once.
		if( removeScoreEntryExactly( scoreEntryId ) )
		{
			m_pendingOperation = QJsonObject{};
			++m_revision;
			if( save() ) { recovered = true; }
			else
			{
				--m_revision;
				m_pendingOperation = operation;
				load( m_className );
				recovered = m_pendingOperation.isEmpty();
			}
		}
	}
	else if( kind == QStringLiteral("reject") )
	{
		bool refundExists = false;
		const int delta = operation.value( QStringLiteral("delta") ).toInt();
		if( delta == 0 || scoreEntryExists( scoreEntryId, &refundExists ) )
		{
			refundExists = delta == 0 || refundExists;
			if( refundExists )
			{
				const auto previousProducts = m_products;
				const auto previousRedemptions = m_redemptions;
				const auto previousRevision = m_revision;
				const auto redemption = ClassroomShopRedemption::fromStorage(
					operation.value( QStringLiteral("redemption") ).toObject() );
				const int index = shopIndexById( m_redemptions, redemption.id );
				bool canCommit = true;
				if( index < 0 || m_redemptions.at( index ).status != ClassroomShopRedemption::Pending )
				{
					if( index < 0 || m_redemptions.at( index ).status != ClassroomShopRedemption::Rejected )
					{
						// The target changed outside this journal. Do not leave a
						// refund attached to a missing/delivered redemption.
						canCommit = removeScoreEntryExactly( scoreEntryId );
					}
				}
				else
				{
					m_redemptions[index].status = ClassroomShopRedemption::Rejected;
					if( redemption.kind == ClassroomShopRedemption::Purchase )
					{
						const int productIndex = shopIndexById( m_products, redemption.productId );
						if( productIndex >= 0 )
						{
							m_products[productIndex].stock = qMin( m_products[productIndex].stock + 1,
								static_cast<int>( MaxStock ) );
						}
					}
				}
				if( canCommit )
				{
					m_pendingOperation = QJsonObject{};
					++m_revision;
					if( save() ) { recovered = true; }
					else
					{
						m_products = previousProducts;
						m_redemptions = previousRedemptions;
						m_revision = previousRevision;
						m_pendingOperation = operation;
						load( m_className );
						recovered = m_pendingOperation.isEmpty();
					}
				}
			}
			else
			{
				// No refund reached the score book, so the rejection never
				// crossed its point of no return and can simply be rolled back.
				m_pendingOperation = QJsonObject{};
				++m_revision;
				if( save() ) { recovered = true; }
				else
				{
					--m_revision;
					m_pendingOperation = operation;
					load( m_className );
					recovered = m_pendingOperation.isEmpty();
				}
			}
		}
	}

	m_recovering = false;
	m_lastError = recovered ? QString{} :
		QStringLiteral( "商城积分操作尚未恢复，已暂停新的积分操作，请稍后重试。" );
	return recovered;
}


const ClassroomShopProduct* ClassroomShopBook::product( const QString& productId ) const
{
	return shopItemById( m_products, productId );
}


QByteArray ClassroomShopBook::productImage( const QString& productId ) const
{
	const auto* item = product( productId );
	return item ? item->image : QByteArray{};
}


const ClassroomShopAuction* ClassroomShopBook::auction( const QString& auctionId ) const
{
	return shopItemById( m_auctions, auctionId );
}


const ClassroomShopRedemption* ClassroomShopBook::redemption( const QString& redemptionId ) const
{
	return shopItemById( m_redemptions, redemptionId );
}


bool ClassroomShopBook::setOpen( bool open )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	if( open == m_open ) { return true; }
	const auto previous = m_open;
	m_open = open;
	++m_revision;
	if( save() ) { return true; }
	m_open = previous;
	--m_revision;
	return false;
}


bool ClassroomShopBook::makeRedemptionCapacity()
{
	while( static_cast<int>( m_redemptions.size() ) >= MaxRedemptions )
	{
		const auto terminal = std::find_if( m_redemptions.begin(), m_redemptions.end(),
			[]( const ClassroomShopRedemption& item ) { return item.status != ClassroomShopRedemption::Pending; } );
		if( terminal == m_redemptions.end() ) { return false; }
		m_redemptions.erase( terminal );
	}
	return true;
}


bool ClassroomShopBook::canMakeRedemptionCapacity() const
{
	if( static_cast<int>( m_redemptions.size() ) < MaxRedemptions ) { return true; }
	return std::any_of( m_redemptions.begin(), m_redemptions.end(),
		[]( const ClassroomShopRedemption& item ) { return item.status != ClassroomShopRedemption::Pending; } );
}


// ------------------------------------------------------------------ catalog

bool ClassroomShopBook::addProduct( ClassroomShopProduct* product )
{
	if( product == nullptr || m_className.isEmpty() || product->name.trimmed().isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	if( static_cast<int>( m_products.size() ) >= MaxProducts ) { return false; }

	ClassroomShopProduct entry = *product;
	entry.id = newShopId();
	entry.name = boundedName( entry.name );
	entry.description = entry.description.trimmed().left( MaxDescriptionLength );
	entry.price = qBound( 0, entry.price, static_cast<int>( MaxPrice ) );
	entry.stock = qBound( 0, entry.stock, static_cast<int>( MaxStock ) );
	if( entry.image.size() > MaxImageBytes ) { entry.image.clear(); }
	entry.imageId = entry.image.isEmpty() ? QString{} : newShopId();

	m_products.append( entry );
	++m_revision;
	if( save() == false )
	{
		m_products.removeLast();
		--m_revision;
		return false;
	}
	*product = entry;
	return true;
}


bool ClassroomShopBook::updateProduct( const ClassroomShopProduct& product )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	const int index = shopIndexById( m_products, product.id );
	if( index < 0 || product.name.trimmed().isEmpty() ) { return false; }

	const auto previous = m_products.at( index );
	auto entry = product;
	entry.name = boundedName( entry.name );
	entry.description = entry.description.trimmed().left( MaxDescriptionLength );
	entry.price = qBound( 0, entry.price, static_cast<int>( MaxPrice ) );
	entry.stock = qBound( 0, entry.stock, static_cast<int>( MaxStock ) );
	if( entry.image.size() > MaxImageBytes ) { entry.image.clear(); }
	if( entry.image != previous.image )
	{
		entry.imageId = entry.image.isEmpty() ? QString{} : newShopId();
	}

	m_products[index] = entry;
	++m_revision;
	if( save() ) { return true; }
	m_products[index] = previous;
	--m_revision;
	return false;
}


bool ClassroomShopBook::removeProduct( const QString& productId )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	const int index = shopIndexById( m_products, productId );
	if( index < 0 ) { return false; }

	// Redemptions keep a copy of the product name, so removing a product
	// never corrupts the delivery queue.
	const auto previous = m_products.at( index );
	m_products.removeAt( index );
	++m_revision;
	if( save() ) { return true; }
	m_products.insert( index, previous );
	--m_revision;
	return false;
}


bool ClassroomShopBook::adjustStock( const QString& productId, int delta )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	const int index = shopIndexById( m_products, productId );
	if( index < 0 ) { return false; }
	const auto previous = m_products.at( index ).stock;
	const auto adjusted = qBound( 0, previous + delta, static_cast<int>( MaxStock ) );
	if( adjusted == previous ) { return true; }

	m_products[index].stock = adjusted;
	++m_revision;
	if( save() ) { return true; }
	m_products[index].stock = previous;
	--m_revision;
	return false;
}


bool ClassroomShopBook::setProductListed( const QString& productId, bool listed )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	const int index = shopIndexById( m_products, productId );
	if( index < 0 || m_products.at( index ).listed == listed ) { return index >= 0; }
	const auto previous = m_products.at( index ).listed;
	m_products[index].listed = listed;
	++m_revision;
	if( save() ) { return true; }
	m_products[index].listed = previous;
	--m_revision;
	return false;
}


QString ClassroomShopBook::startAuction( const QString& name, int startPrice, int minIncrement )
{
	if( m_className.isEmpty() || name.trimmed().isEmpty() ) { return {}; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return {};
	}
	int openCount = 0;
	for( const auto& auction : m_auctions )
	{
		if( auction.open ) { ++openCount; }
	}
	if( openCount >= MaxOpenAuctions ) { return {}; }

	int recycleIndex = -1;
	ClassroomShopAuction recycled;
	if( static_cast<int>( m_auctions.size() ) >= MaxStoredAuctions )
	{
		for( int index = 0; index < m_auctions.size(); ++index )
		{
			if( m_auctions.at( index ).open == false )
			{
				recycleIndex = index;
				recycled = m_auctions.at( index );
				break;
			}
		}
		if( recycleIndex < 0 ) { return {}; }
		m_auctions.removeAt( recycleIndex );
	}

	ClassroomShopAuction auction;
	auction.id = newShopId();
	auction.name = boundedName( name );
	auction.startPrice = qBound( 1, startPrice, static_cast<int>( MaxPrice ) );
	auction.minIncrement = qBound( 1, minIncrement, static_cast<int>( MaxIncrement ) );
	auction.open = true;

	m_auctions.append( auction );
	++m_revision;
	if( save() == false )
	{
		m_auctions.removeLast();
		if( recycleIndex >= 0 ) { m_auctions.insert( recycleIndex, recycled ); }
		--m_revision;
		return {};
	}
	return auction.id;
}


ClassroomShopAuctionResult ClassroomShopBook::closeAuction( const QString& auctionId )
{
	ClassroomShopAuctionResult result;
	if( m_className.isEmpty() ) { return result; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		result.error = QStringLiteral( "无法加载或锁定商城数据。" );
		return result;
	}
	if( recoverPendingOperationsLocked() == false )
	{
		result.error = lastError();
		return result;
	}
	const int index = shopIndexById( m_auctions, auctionId );
	if( index < 0 || m_auctions.at( index ).open == false ) { return result; }

	result.found = true;
	result.auctionId = auctionId;
	result.name = m_auctions.at( index ).name;

	// Without a bid the auction simply closes unsold ("流拍").
	if( m_auctions.at( index ).bids.isEmpty() )
	{
		m_auctions[index].open = false;
		++m_revision;
		if( save() == false )
		{
			m_auctions[index].open = true;
			--m_revision;
			result = ClassroomShopAuctionResult{};
			result.error = QStringLiteral( "商城数据保存失败，竞拍保持进行中，请稍后重试。" );
		}
		return result;
	}

	// 价高者得: bids are stored in ascending order, so the last one wins.
	const auto winning = m_auctions.at( index ).bids.last();
	result.price = winning.amount;
	result.winnerSeat = winning.seat;
	result.winnerStudent = winning.student;

	ClassroomRoster roster( m_basePath );
	const auto* currentStudent = roster.findStudent( m_className, winning.seat );
	if( currentStudent == nullptr || currentStudent->name != winning.student )
	{
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "该座位的学生身份已变化，竞拍保持进行中；请取消竞拍或核对名册。" );
		return result;
	}

	const auto available = ClassroomShopBook::spendableScore( balance( winning.seat ) );
	if( available < winning.amount )
	{
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "最高出价者当前可用积分不足（需保留 %1 分），竞拍保持进行中；请先处理其积分后再结束。" ).
			arg( ClassroomShopBook::ProtectedScore );
		return result;
	}
	if( canMakeRedemptionCapacity() == false )
	{
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "待发货队列已满，请先处理发货记录，竞拍保持进行中。" );
		return result;
	}

	ClassroomShopRedemption redemption;
	redemption.id = newShopId();
	redemption.productName = QStringLiteral( "竞拍成交：%1" ).arg( result.name );
	redemption.seat = winning.seat;
	redemption.student = winning.student;
	redemption.price = winning.amount;
	redemption.kind = ClassroomShopRedemption::AuctionWin;
	redemption.timestamp = QDateTime::currentDateTime();

	const auto scoreEntryId = newShopId();
	const auto label = QStringLiteral( "竞拍拍得：%1" ).arg( result.name );
	const QJsonObject operation{
		{ QStringLiteral("version"), ShopJournalVersion },
		{ QStringLiteral("id"), newShopId() },
		{ QStringLiteral("kind"), QStringLiteral("auction") },
		{ QStringLiteral("auctionId"), auctionId },
		{ QStringLiteral("scoreEntryId"), scoreEntryId },
		{ QStringLiteral("seat"), winning.seat },
		{ QStringLiteral("student"), winning.student },
		{ QStringLiteral("delta"), -winning.amount },
		{ QStringLiteral("label"), label },
		{ QStringLiteral("redemption"), redemption.toStorage() },
		{ QStringLiteral("created"), redemption.timestamp.toString( Qt::ISODateWithMs ) } };
	if( preparePendingOperation( operation ) == false )
	{
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "无法保存竞拍操作日志，竞拍保持进行中。%1" ).arg( lastError() );
		return result;
	}

	ClassroomScoreEntry charge;
	charge.id = scoreEntryId;
	charge.timestamp = redemption.timestamp;
	charge.className = m_className;
	charge.seat = winning.seat;
	charge.student = winning.student;
	charge.delta = -winning.amount;
	charge.label = label;
	charge.author = lessonTopic();
	if( appendScoreEntry( charge ) == false )
	{
		recoverPendingOperationsLocked();
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "无法写入积分记录，竞拍保持进行中，请稍后重试。%1" ).arg( lastError() );
		return result;
	}

	const auto previousRedemptions = m_redemptions;
	const auto previousRevision = m_revision;
	m_auctions[index].open = false;
	if( makeRedemptionCapacity() == false )
	{
		m_auctions[index].open = true;
		m_redemptions = previousRedemptions;
		m_revision = previousRevision;
		recoverPendingOperationsLocked();
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "待发货队列已满，竞拍保持进行中。%1" ).arg( lastError() );
		return result;
	}
	m_redemptions.append( redemption );
	++m_revision;
	m_pendingOperation = QJsonObject{};
	if( save() )
	{
		result.sold = true;
		return result;
	}

	// Reconcile an ambiguous commit before touching the score again. If the
	// final atomic save won, the auction is closed and the journal is gone.
	m_auctions[index].open = true;
	m_redemptions = previousRedemptions;
	m_revision = previousRevision;
	m_pendingOperation = operation;
	load( m_className );
	const auto* committedAuction = auction( auctionId );
	if( m_loaded && m_pendingOperation.isEmpty() && committedAuction != nullptr &&
		committedAuction->open == false && shopItemById( m_redemptions, redemption.id ) != nullptr )
	{
		result.sold = true;
		return result;
	}
	recoverPendingOperationsLocked();
	const auto* recoveredAuction = auction( auctionId );
	if( m_pendingOperation.isEmpty() && recoveredAuction != nullptr &&
		recoveredAuction->open == false && shopItemById( m_redemptions, redemption.id ) != nullptr )
	{
		result.sold = true;
		return result;
	}
	result = ClassroomShopAuctionResult{};
	result.error = QStringLiteral( "商城数据保存失败，竞拍未成交；请稍后重试。%1" ).arg( lastError() );
	return result;
}


bool ClassroomShopBook::removeAuction( const QString& auctionId )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	const int index = shopIndexById( m_auctions, auctionId );
	if( index < 0 ) { return false; }

	// Deleting an open auction cancels it: nobody pays.
	const auto previous = m_auctions.at( index );
	m_auctions.removeAt( index );
	++m_revision;
	if( save() ) { return true; }
	m_auctions.insert( index, previous );
	--m_revision;
	return false;
}


QString ClassroomShopBook::redeem( const QString& productId, const QString& seat,
								   const QString& student, int balance,
								   const QString& operationId )
{
	if( m_className.isEmpty() ) { return QStringLiteral( "班级不存在。" ); }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		return QStringLiteral( "无法加载或锁定商城数据。" );
	}
	if( recoverPendingOperationsLocked() == false ) { return lastError(); }
	const auto normalizedSeat = seat.trimmed().left( 16 );
	const auto normalizedStudent = student.trimmed().left( 32 );
	const auto stableOperationId = operationId.trimmed().left( 128 );
	const auto fingerprint = redeemFingerprint( productId, normalizedSeat, normalizedStudent );
	if( stableOperationId.isEmpty() == false )
	{
		for( const auto& value : m_completedOperations )
		{
			const auto completed = value.toObject();
			if( completed.value( QStringLiteral("id") ).toString() != stableOperationId ) { continue; }
			if( completed.value( QStringLiteral("fingerprint") ).toString() != fingerprint )
			{
				return QStringLiteral( "该操作编号已用于其他兑换请求。" );
			}
			return {};
		}
	}

	if( normalizedSeat.isEmpty() ) { return QStringLiteral( "请先签到后再兑换。" ); }
	if( m_open == false ) { return QStringLiteral( "老师暂时关闭了商场，请稍后再来。" ); }

	const int index = shopIndexById( m_products, productId );
	if( index < 0 || m_products.at( index ).listed == false ) { return QStringLiteral( "商品不存在或已下架。" ); }

	const auto& product = m_products.at( index );
	if( product.stock <= 0 ) { return QStringLiteral( "「%1」已经兑完了。" ).arg( product.name ); }
	const auto available = spendableScore( balance );
	if( available < product.price )
	{
		return QStringLiteral( "可用积分不足：需要 %1 分，你当前可用 %2 分（总积分 %3，保留 %4 分）。" ).
			arg( product.price ).arg( available ).arg( balance ).arg( ProtectedScore );
	}
	if( canMakeRedemptionCapacity() == false )
	{
		return QStringLiteral( "待发货队列已满，请先让老师处理发货记录。" );
	}

	ClassroomShopRedemption redemption;
	redemption.id = newShopId();
	redemption.productId = product.id;
	redemption.productName = product.name;
	redemption.seat = normalizedSeat;
	redemption.student = normalizedStudent;
	redemption.price = product.price;
	redemption.kind = ClassroomShopRedemption::Purchase;
	redemption.timestamp = QDateTime::currentDateTime();

	const auto scoreEntryId = newShopId();
	const auto label = QStringLiteral( "兑换：%1" ).arg( product.name );
	const QJsonObject operation{
		{ QStringLiteral("version"), ShopJournalVersion },
		{ QStringLiteral("id"), stableOperationId.isEmpty() ? newShopId() : stableOperationId },
		{ QStringLiteral("kind"), QStringLiteral("purchase") },
		{ QStringLiteral("fingerprint"), fingerprint },
		{ QStringLiteral("scoreEntryId"), scoreEntryId },
		{ QStringLiteral("seat"), normalizedSeat },
		{ QStringLiteral("student"), normalizedStudent },
		{ QStringLiteral("delta"), -product.price },
		{ QStringLiteral("label"), label },
		{ QStringLiteral("redemption"), redemption.toStorage() },
		{ QStringLiteral("created"), redemption.timestamp.toString( Qt::ISODateWithMs ) } };
	if( preparePendingOperation( operation ) == false )
	{
		return QStringLiteral( "无法保存兑换操作日志，请稍后重试。%1" ).arg( lastError() );
	}

	ClassroomScoreEntry charge;
	charge.id = scoreEntryId;
	charge.timestamp = redemption.timestamp;
	charge.className = m_className;
	charge.seat = normalizedSeat;
	charge.student = normalizedStudent;
	charge.delta = -product.price;
	charge.label = label;
	charge.author = lessonTopic();
	if( appendScoreEntry( charge ) == false )
	{
		recoverPendingOperationsLocked();
		return QStringLiteral( "无法写入积分记录，请稍后重试。%1" ).arg( lastError() );
	}

	const auto previousRedemptions = m_redemptions;
	const auto previousCompletedOperations = m_completedOperations;
	const auto previousRevision = m_revision;
	--m_products[index].stock;
	if( makeRedemptionCapacity() == false )
	{
		++m_products[index].stock;
		m_redemptions = previousRedemptions;
		m_revision = previousRevision;
		recoverPendingOperationsLocked();
		return QStringLiteral( "待发货队列已满，请稍后重试。%1" ).arg( lastError() );
	}
	m_redemptions.append( redemption );
	if( stableOperationId.isEmpty() == false )
	{
		m_completedOperations.append( QJsonObject{
			{ QStringLiteral("id"), stableOperationId },
			{ QStringLiteral("fingerprint"), fingerprint },
			{ QStringLiteral("redemptionId"), redemption.id },
			{ QStringLiteral("ts"), redemption.timestamp.toString( Qt::ISODateWithMs ) } } );
		while( m_completedOperations.size() > MaxCompletedShopOperations )
		{
			m_completedOperations.removeFirst();
		}
	}
	++m_revision;
	m_pendingOperation = QJsonObject{};
	if( save() ) { return {}; }

	// The final save contains both the stock/redemption mutation and the
	// idempotency record. Reconcile before reporting failure or rolling back.
	++m_products[index].stock;
	m_products[index].stock = qMin( m_products[index].stock, static_cast<int>( MaxStock ) );
	m_redemptions = previousRedemptions;
	m_completedOperations = previousCompletedOperations;
	m_revision = previousRevision;
	m_pendingOperation = operation;
	load( m_className );
	if( m_pendingOperation.isEmpty() && shopItemById( m_redemptions, redemption.id ) != nullptr )
	{
		return {};
	}
	recoverPendingOperationsLocked();
	if( m_pendingOperation.isEmpty() && shopItemById( m_redemptions, redemption.id ) != nullptr )
	{
		return {};
	}
	return QStringLiteral( "商城数据保存失败，兑换未完成；请稍后重试。%1" ).arg( lastError() );
}


QString ClassroomShopBook::placeBid( const QString& auctionId, int amount, const QString& seat,
									 const QString& student, int balance )
{
	if( seat.trimmed().isEmpty() ) { return QStringLiteral( "请先签到后再竞拍。" ); }
	if( m_className.isEmpty() ) { return QStringLiteral( "班级不存在。" ); }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		return QStringLiteral( "无法加载或锁定商城数据。" );
	}
	if( m_open == false ) { return QStringLiteral( "老师暂时关闭了商场，请稍后再来。" ); }

	const int index = shopIndexById( m_auctions, auctionId );
	if( index < 0 ) { return QStringLiteral( "竞拍不存在。" ); }

	auto& auction = m_auctions[index];
	if( auction.open == false ) { return QStringLiteral( "该竞拍已经结束。" ); }
	if( amount > MaxBid ) { return QStringLiteral( "出价超出范围。" ); }
	const auto minimum = auction.minNextBid();
	if( amount < minimum ) { return QStringLiteral( "出价至少为 %1 分。" ).arg( minimum ); }
	const auto available = spendableScore( balance );
	if( amount > available )
	{
		return QStringLiteral( "可用积分不足：你当前可用 %1 分（总积分 %2，保留 %3 分），出不了 %4 分。" ).
			arg( available ).arg( balance ).arg( ProtectedScore ).arg( amount );
	}
	if( static_cast<int>( auction.bids.size() ) >= MaxStoredBids )
	{
		return QStringLiteral( "本场竞拍的出价次数已达上限。" );
	}

	ClassroomShopBid bid;
	bid.seat = seat.trimmed().left( 16 );
	bid.student = student.trimmed().left( 32 );
	bid.amount = amount;
	bid.time = QDateTime::currentDateTime();
	auction.bids.append( bid );
	++m_revision;
	if( save() == false )
	{
		auction.bids.removeLast();
		--m_revision;
		return QStringLiteral( "商城数据保存失败，请稍后重试。" );
	}
	return {};
}


bool ClassroomShopBook::setRedemptionStatus( const QString& redemptionId, int status )
{
	if( m_className.isEmpty() ) { return false; }
	QLockFile lock( filePath() + QStringLiteral( ".lock" ) );
	if( tryLockShopFile( lock ) == false || load( m_className ) == false )
	{
		m_lastError = QStringLiteral( "无法加载或锁定商城数据。" );
		return false;
	}
	if( recoverPendingOperationsLocked() == false ) { return false; }
	const int index = shopIndexById( m_redemptions, redemptionId );
	if( index < 0 ) { return false; }

	const auto previous = m_redemptions.at( index );
	if( previous.status == status ) { return true; }
	if( previous.status != ClassroomShopRedemption::Pending ||
		( status != ClassroomShopRedemption::Delivered &&
		  status != ClassroomShopRedemption::Rejected ) )
	{
		return false;
	}

	const bool rejecting = status == ClassroomShopRedemption::Rejected;
	if( rejecting == false )
	{
		m_redemptions[index].status = status;
		++m_revision;
		if( save() ) { return true; }
		m_redemptions[index] = previous;
		--m_revision;
		m_lastError = QStringLiteral( "发货状态保存失败。" );
		return false;
	}

	ClassroomRoster roster( m_basePath );
	const auto* currentStudent = roster.findStudent( m_className, previous.seat );
	if( currentStudent == nullptr || currentStudent->name != previous.student )
	{
		m_lastError = QStringLiteral( "座位学生身份已变化，无法退款。" );
		return false;
	}

	const auto scoreEntryId = newShopId();
	const auto label = QStringLiteral( "退回：%1" ).arg( previous.productName );
	const QJsonObject operation{
		{ QStringLiteral("version"), ShopJournalVersion },
		{ QStringLiteral("id"), newShopId() },
		{ QStringLiteral("kind"), QStringLiteral("reject") },
		{ QStringLiteral("redemptionId"), redemptionId },
		{ QStringLiteral("scoreEntryId"), scoreEntryId },
		{ QStringLiteral("seat"), previous.seat },
		{ QStringLiteral("student"), previous.student },
		{ QStringLiteral("delta"), previous.price },
		{ QStringLiteral("label"), label },
		{ QStringLiteral("redemption"), previous.toStorage() },
		{ QStringLiteral("created"), QDateTime::currentDateTime().toString( Qt::ISODateWithMs ) } };
	if( preparePendingOperation( operation ) == false )
	{
		m_lastError = QStringLiteral( "无法保存退款操作日志。%1" ).arg( lastError() );
		return false;
	}

	ClassroomScoreEntry refund;
	refund.id = scoreEntryId;
	refund.timestamp = QDateTime::currentDateTime();
	refund.className = m_className;
	refund.seat = previous.seat;
	refund.student = previous.student;
	refund.delta = previous.price;
	refund.label = label;
	refund.author = lessonTopic();
	if( appendScoreEntry( refund ) == false )
	{
		recoverPendingOperationsLocked();
		const auto* current = redemption( redemptionId );
		if( m_pendingOperation.isEmpty() && current != nullptr &&
			current->status == ClassroomShopRedemption::Rejected )
		{
			return true;
		}
		m_lastError = QStringLiteral( "退款写入失败，请勿重复操作，稍后重试。%1" ).arg( lastError() );
		return false;
	}

	const auto previousProducts = m_products;
	const auto previousRedemptions = m_redemptions;
	const auto previousRevision = m_revision;
	m_redemptions[index].status = ClassroomShopRedemption::Rejected;
	if( previous.kind == ClassroomShopRedemption::Purchase )
	{
		const int productIndex = shopIndexById( m_products, previous.productId );
		if( productIndex >= 0 )
		{
			m_products[productIndex].stock = qMin( m_products[productIndex].stock + 1,
				static_cast<int>( MaxStock ) );
		}
	}
	++m_revision;
	m_pendingOperation = QJsonObject{};
	if( save() ) { return true; }

	m_products = previousProducts;
	m_redemptions = previousRedemptions;
	m_revision = previousRevision;
	m_pendingOperation = operation;
	load( m_className );
	const auto* current = redemption( redemptionId );
	if( m_pendingOperation.isEmpty() && current != nullptr &&
		current->status == ClassroomShopRedemption::Rejected )
	{
		return true;
	}
	recoverPendingOperationsLocked();
	const auto* recoveredRedemption = redemption( redemptionId );
	if( m_pendingOperation.isEmpty() && recoveredRedemption != nullptr &&
		recoveredRedemption->status == ClassroomShopRedemption::Rejected )
	{
		return true;
	}
	m_lastError = QStringLiteral( "退款或商城状态保存失败，请勿重复操作，稍后重试。%1" ).arg( lastError() );
	return false;
}


// ------------------------------------------------------------------- scores

int ClassroomShopBook::balance( const QString& seat ) const
{
	if( m_className.isEmpty() || seat.isEmpty() ) { return 0; }
	ClassroomScoreBook book( m_basePath );
	if( book.load( m_className ) == false ) { return 0; }
	return book.total( seat );
}


bool ClassroomShopBook::bookScore( const QString& seat, const QString& student,
								   int delta, const QString& label )
{
	if( delta == 0 ) { return true; }
	if( m_className.isEmpty() || seat.trimmed().isEmpty() ) { return false; }

	ClassroomScoreEntry entry;
	entry.className = m_className;
	entry.seat = seat.trimmed();
	entry.student = student.trimmed();
	entry.delta = delta;
	entry.label = label.left( 200 );
	entry.author = lessonTopic();
	return appendScoreEntry( entry );
}


// ----------------------------------------------------------------- payloads

QJsonObject ClassroomShopBook::catalogJson( bool withImages ) const
{
	QJsonArray products;
	for( const auto& product : m_products )
	{
		if( product.listed ) { products.append( product.toJson( withImages ) ); }
	}
	QJsonArray auctions;
	for( const auto& auction : m_auctions ) { auctions.append( auction.toJson() ); }
	return { { QStringLiteral("revision"), m_revision },
			 { QStringLiteral("open"), m_open },
			 { QStringLiteral("protectedScore"), ProtectedScore },
			 { QStringLiteral("products"), products },
			 { QStringLiteral("auctions"), auctions } };
}


QByteArray ClassroomShopBook::catalogImage( const QImage& image )
{
	if( image.isNull() ) { return {}; }
	const auto format = image.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32;
	for( const int size : { 256, 192, 144, 112, 80, 56 } )
	{
		const auto scaled = image.scaled( size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation )
								.convertToFormat( format );
		QByteArray bytes;
		QBuffer buffer( &bytes );
		buffer.open( QIODevice::WriteOnly );
		scaled.save( &buffer, "PNG" );
		buffer.close();
		if( bytes.isEmpty() == false && bytes.size() <= MaxImageBytes ) { return bytes; }
	}
	return {};
}
