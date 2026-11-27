/*
 * ClassroomShop.cpp - per-class shop and auction book on the teacher machine
 *
 * Every mutation follows the same order: book the score entry first, then
 * save the shop file. If the save fails the score entry is reversed again,
 * so a student is never charged for something the book did not record. All
 * callers run on the teacher GUI thread, so there is no concurrent writer.
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
#include <QSaveFile>
#include <QUuid>

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
		ClassroomLogStore::sanitize( m_className ) + QStringLiteral(".json") );
}


bool ClassroomShopBook::load( const QString& className )
{
	m_className = className;
	m_loaded = false;
	m_open = true;
	m_revision = 0;
	m_products.clear();
	m_auctions.clear();
	m_redemptions.clear();
	if( m_className.isEmpty() ) { m_loaded = true; return true; }

	bool ok = true;
	const auto data = readShopFile( filePath(), &ok );
	if( ok == false ) { return false; }
	if( data.isEmpty() && QFileInfo::exists( filePath() ) ) { return false; }
	if( data.isEmpty() == false )
	{
		const auto document = QJsonDocument::fromJson( data );
		if( document.isObject() == false ) { return false; }
		const auto object = document.object();
		m_open = object.value( QStringLiteral("open") ).toBool( true );
		m_revision = qMax<qint64>( 0, object.value( QStringLiteral("revision") ).toInteger() );
		for( const auto& value : object.value( QStringLiteral("products") ).toArray() )
		{
			if( static_cast<int>( m_products.size() ) >= MaxProducts ) { return false; }
			m_products.append( ClassroomShopProduct::fromJson( value.toObject() ) );
		}
		for( const auto& value : object.value( QStringLiteral("auctions") ).toArray() )
		{
			if( static_cast<int>( m_auctions.size() ) >= ClassroomShopBook::MaxStoredAuctions ) { return false; }
			m_auctions.append( ClassroomShopAuction::fromStorage( value.toObject() ) );
		}
		for( const auto& value : object.value( QStringLiteral("redemptions") ).toArray() )
		{
			if( static_cast<int>( m_redemptions.size() ) >= MaxRedemptions ) { return false; }
			m_redemptions.append( ClassroomShopRedemption::fromStorage( value.toObject() ) );
		}
	}
	m_loaded = true;
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
		{ QStringLiteral("redemptions"), storageArray( m_redemptions ) } };
	return writeShopFile( filePath(), QJsonDocument( object ).toJson( QJsonDocument::Indented ) );
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
	if( m_className.isEmpty() || open == m_open ) { return true; }
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


// ------------------------------------------------------------------ catalog

bool ClassroomShopBook::addProduct( ClassroomShopProduct* product )
{
	if( product == nullptr || m_className.isEmpty() || product->name.trimmed().isEmpty() ) { return false; }
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
	result.sold = true;
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

	if( bookScore( winning.seat, winning.student, -winning.amount,
				   QStringLiteral( "竞拍拍得：%1" ).arg( result.name ) ) == false )
	{
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "无法写入积分记录，竞拍保持进行中，请稍后重试。" );
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

	const auto previousRedemptions = m_redemptions;
	m_auctions[index].open = false;
	if( makeRedemptionCapacity() == false )
	{
		m_auctions[index].open = true;
		m_redemptions = previousRedemptions;
		bookScore( winning.seat, winning.student, winning.amount,
				   QStringLiteral( "竞拍未成交，退回：%1" ).arg( result.name ) );
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "待发货队列已满，请先处理发货记录，竞拍保持进行中。" );
		return result;
	}
	m_redemptions.append( redemption );
	++m_revision;
	if( save() == false )
	{
		// Undo everything: the points go back and the auction stays open.
		m_auctions[index].open = true;
		m_redemptions = previousRedemptions;
		--m_revision;
		bookScore( winning.seat, winning.student, winning.amount,
				   QStringLiteral( "竞拍未成交，退回：%1" ).arg( result.name ) );
		result = ClassroomShopAuctionResult{};
		result.error = QStringLiteral( "商城数据保存失败，竞拍保持进行中，请稍后重试。" );
	}
	return result;
}


bool ClassroomShopBook::removeAuction( const QString& auctionId )
{
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
								   const QString& student, int balance )
{
	if( seat.trimmed().isEmpty() ) { return QStringLiteral( "请先签到后再兑换。" ); }
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
	const auto previousRedemptions = m_redemptions;
	if( makeRedemptionCapacity() == false )
	{
		m_redemptions = previousRedemptions;
		return QStringLiteral( "待发货队列已满，请先让老师处理发货记录。" );
	}

	// Score first, stock second: a failed save refunds the student again.
	const auto label = QStringLiteral( "兑换：%1" ).arg( product.name );
	if( bookScore( seat, student, -product.price, label ) == false )
	{
		m_redemptions = previousRedemptions;
		return QStringLiteral( "无法写入积分记录，请稍后重试。" );
	}

	ClassroomShopRedemption redemption;
	redemption.id = newShopId();
	redemption.productId = product.id;
	redemption.productName = product.name;
	redemption.seat = seat.trimmed().left( 16 );
	redemption.student = student.trimmed().left( 32 );
	redemption.price = product.price;
	redemption.kind = ClassroomShopRedemption::Purchase;
	redemption.timestamp = QDateTime::currentDateTime();

	--m_products[index].stock;
	m_redemptions.append( redemption );
	++m_revision;
	if( save() == false )
	{
		++m_products[index].stock;
		m_redemptions = previousRedemptions;
		--m_revision;
		bookScore( seat, student, product.price, QStringLiteral( "兑换未成功，退回：%1" ).arg( product.name ) );
		return QStringLiteral( "商城数据保存失败，请稍后重试。" );
	}
	return {};
}


QString ClassroomShopBook::placeBid( const QString& auctionId, int amount, const QString& seat,
									 const QString& student, int balance )
{
	if( seat.trimmed().isEmpty() ) { return QStringLiteral( "请先签到后再竞拍。" ); }
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

	// Only a pending rejection gives anything back: it always refunds the price and a
	// shop purchase also returns the unit to the shelf. Auction wins are never
	// restocked because the item was never taken from the catalog.
	const bool rejecting = status == ClassroomShopRedemption::Rejected;
	if( rejecting )
	{
		ClassroomRoster roster( m_basePath );
		const auto* currentStudent = roster.findStudent( m_className, previous.seat );
		if( currentStudent == nullptr || currentStudent->name != previous.student ) { return false; }
	}
	if( rejecting && bookScore( previous.seat, previous.student, previous.price,
								QStringLiteral( "退回：%1" ).arg( previous.productName ) ) == false )
	{
		return false;
	}

	int restocked = -1;
	int previousStock = 0;
	if( rejecting && previous.kind == ClassroomShopRedemption::Purchase )
	{
		for( int productIndex = 0; productIndex < m_products.size(); ++productIndex )
		{
			if( m_products.at( productIndex ).id == previous.productId )
			{
				restocked = productIndex;
				previousStock = m_products.at( productIndex ).stock;
				m_products[productIndex].stock = qMin( previousStock + 1, static_cast<int>( MaxStock ) );
				break;
			}
		}
	}

	m_redemptions[index].status = status;
	++m_revision;
	if( save() ) { return true; }

	// Save failed: put everything back, including the refund.
	m_redemptions[index] = previous;
	if( restocked >= 0 ) { m_products[restocked].stock = previousStock; }
	--m_revision;
	if( rejecting )
	{
		bookScore( previous.seat, previous.student, -previous.price,
				   QStringLiteral( "撤销退款：%1" ).arg( previous.productName ) );
	}
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

	ClassroomScoreBook book( m_basePath );
	if( book.load( m_className ) == false ) { return false; }

	// One lesson per day keeps every shop entry of that day together and makes
	// it visible in "我的成绩" as "MM.dd 班级商场".
	ClassroomScoreEntry entry;
	entry.lessonId = book.ensureLesson( QDate::currentDate(), lessonTopic() );
	entry.className = m_className;
	entry.seat = seat.trimmed();
	entry.student = student.trimmed();
	entry.delta = delta;
	entry.label = label.left( 200 );
	entry.author = lessonTopic();
	return book.addEntry( entry );
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
