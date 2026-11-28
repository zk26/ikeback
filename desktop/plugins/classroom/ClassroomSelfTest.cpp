/*
 * ClassroomSelfTest.cpp - dependency free checks for the classroom data layer
 *
 * Built only with WITH_TESTS=ON via test_ikeback_plugin(classroom ClassroomSelfTest).
 * This file is part of the Ikeback classroom modification.
 */

#include <QGuiApplication>
#include <QImage>
#include <QDate>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTextStream>

#include "ClassroomBridge.h"
#include "ClassroomConfiguration.h"
#include "ClassroomData.h"
#include "ClassroomProtocol.h"
#include "ClassroomScore.h"
#include "ClassroomSecrets.h"
#include "ClassroomShop.h"
#include "ClassroomTermReport.h"
#include "ClassroomWordCloud.h"

#include <algorithm>
#include <cstdio>


static int failures = 0;

static void check( bool condition, const char* what )
{
	if( condition == false )
	{
		++failures;
		std::fprintf( stderr, "FAIL: %s\n", what );
	}
	else
	{
		std::fprintf( stdout, "ok  : %s\n", what );
	}
}


int main( int argc, char** argv )
{
	QGuiApplication app( argc, argv );
	QTemporaryDir directory;
	check( directory.isValid(), "temporary data directory" );

	check( ClassroomScoreBook::xmlEscape( QStringLiteral("a<b>&\"") ) ==
		   QStringLiteral("a&lt;b&gt;&amp;&quot;"), "xml escaping" );
	check( ClassroomSecrets::reveal( QStringLiteral("plain-key") ) == QStringLiteral("plain-key"),
		   "legacy plain secrets stay readable" );
	check( ClassroomSecrets::defaultAdminKey().isEmpty() &&
		   ClassroomSecrets::defaultAdminKeyHash().isEmpty() &&
		   ClassroomSecrets::verifyAdminKey( QStringLiteral("Admin@123"), QString{} ) == false,
		   "student administrator authentication fails closed without a configured password" );
	const auto configuredAdminHash = ClassroomSecrets::adminKeyHash( QStringLiteral("teacher-only-secret") );
	check( configuredAdminHash.isEmpty() == false &&
		   ClassroomSecrets::verifyAdminKey( QStringLiteral("teacher-only-secret"), configuredAdminHash ) &&
		   ClassroomSecrets::verifyAdminKey( QStringLiteral("wrong"), configuredAdminHash ) == false &&
		   ClassroomSecrets::verifyAdminKey( QStringLiteral("Admin@123"),
				QString::fromLatin1( QCryptographicHash::hash( QByteArrayLiteral("Admin@123"),
					QCryptographicHash::Sha256 ).toHex() ) ) == false,
		   "configured administrator passwords verify and the retired default is rejected" );
	const auto defaultStudentSettings = ClassroomSettings::forStudents( QJsonObject{} );
	check( defaultStudentSettings.value( Classroom::SettingKey::adminKeyHash() ).toString().isEmpty() &&
		   defaultStudentSettings.contains( Classroom::SettingKey::adminKey() ) == false,
		   "student settings never carry a default password or plain password" );
	// Shared HMAC reference vector with mobile/IkebackCryptoTest.
	const auto referenceToken = ClassroomSecrets::requestToken(
		QStringLiteral("ABC123"), QStringLiteral("GET"), QStringLiteral("/api/classroom/state"),
		QStringLiteral("nonce-0001"), QByteArray() );
	check( referenceToken == QByteArrayLiteral("bd41d1e94c8c04a9ae857dce47c9f1d6907b65cfcd5c1bcbd8d08711a7c14e50"),
		   "request-bound HMAC matches the Kotlin reference" );
	const auto referenceResponse = ClassroomSecrets::responseToken(
		QStringLiteral("ABC123"), QStringLiteral("nonce-0002"),
		QStringLiteral("1700000000"), QByteArrayLiteral("{\"ok\":true}") );
	check( referenceResponse ==
		   QByteArrayLiteral("b35ec4ba272a86a16ecc54d358f4ff721e7e1e816aabfa55c0f6f631d45483cb"),
		   "response HMAC matches the Kotlin reference over exact bytes" );
	check( referenceToken.contains( QByteArrayLiteral("ABC123") ) == false,
		   "auth token never contains the plain lesson key" );
	check( ClassroomSecrets::requestToken( QStringLiteral("ABC123"), QStringLiteral("POST"),
										  QStringLiteral("/api/classroom/state"),
										  QStringLiteral("nonce-0001"), QByteArray() ) != referenceToken &&
		   ClassroomSecrets::requestToken( QStringLiteral("ABC123"), QStringLiteral("GET"),
										  QStringLiteral("/api/classroom/events"),
										  QStringLiteral("nonce-0001"), QByteArray() ) != referenceToken &&
		   ClassroomSecrets::requestToken( QStringLiteral("ABC123"), QStringLiteral("GET"),
										  QStringLiteral("/api/classroom/state"),
										  QStringLiteral("nonce-0002"), QByteArray() ) != referenceToken &&
		   ClassroomSecrets::requestToken( QStringLiteral("ABC123"), QStringLiteral("POST"),
										  QStringLiteral("/api/classroom/score"),
										  QStringLiteral("nonce-0001"), QByteArrayLiteral("{}") ) !=
			   ClassroomSecrets::requestToken( QStringLiteral("ABC123"), QStringLiteral("POST"),
											  QStringLiteral("/api/classroom/score"),
											  QStringLiteral("nonce-0001"), QByteArrayLiteral("[]") ),
		   "HMAC is bound to method, path, nonce and body" );
	check( Classroom::announcementPrefix() == QStringLiteral("IKEBACK:") &&
		   Classroom::discoveryMagic() == QStringLiteral("IKEBACK_DISCOVER"),
		   "mobile discovery constants match the Kotlin app" );

	// Bridge request parsing: half packets, oversized bodies and bad inputs.
	const QByteArray header = QByteArrayLiteral("POST /api/classroom/score HTTP/1.1\r\nContent-Length: 7\r\n\r\n");
	const QByteArray body = QByteArrayLiteral("{\"a\":1}");
	const auto half = ClassroomBridge::parseRequest( header + body.left( 3 ) );
	check( half.complete == false && half.tooLarge == false, "partial TCP body waits for more data" );
	const auto whole = ClassroomBridge::parseRequest( header + body );
	check( whole.complete && whole.method == QStringLiteral("POST") && whole.body == body,
		   "complete request keeps the exact body" );
	check( ClassroomBridge::parseRequest(
			   QByteArrayLiteral("POST /x HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n") ).tooLarge,
		   "body above 1 MiB is rejected before authentication" );
	check( ClassroomBridge::parseRequest(
			   QByteArrayLiteral("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") ).tooLarge,
		   "chunked encoding is rejected" );
	check( ClassroomBridge::sanitizeTeacherIp( QStringLiteral("0.0.0.0/0") ).isEmpty() &&
		   ClassroomBridge::sanitizeTeacherIp( QStringLiteral("0.0.0.0") ).isEmpty() &&
		   ClassroomBridge::sanitizeTeacherIp( QStringLiteral("127.0.0.1") ).isEmpty() &&
		   ClassroomBridge::sanitizeTeacherIp( QStringLiteral("192.168.1.2") ) == QStringLiteral("192.168.1.2"),
		   "teacherIp whitelist rejects ranges and loopback" );
	check( ClassroomBridge::validDelta( -5 ) && ClassroomBridge::validDelta( 5 ) &&
		   ClassroomBridge::validDelta( 6 ) == false && ClassroomBridge::validDelta( 0 ) == false,
		   "score delta is limited to -5..5" );

	const QString imageHtml = QStringLiteral("<img src=\"logo.png\" /> logo.png in text");
	check( ClassroomScoreBook::inlineImages( imageHtml, directory.path() ) ==
		   QStringLiteral("<img src=\"logo.png\" /> logo.png in text"),
		   "missing image is left untouched (no blind string replacement)" );

	ClassroomScoreBook book( directory.path() );
	book.load( QStringLiteral("Class A") );
	ClassroomScoreRules rules;
	rules.baseScore = 100;
	book.setRules( rules );

	// Lessons are inserted out of chronological order on purpose.
	const QString newer = book.ensureLesson( QDate( 2026, 11, 4 ), QStringLiteral("WPS") );
	const QString older = book.ensureLesson( QDate( 2026, 11, 3 ), QStringLiteral("Drawing") );
	check( newer.isEmpty() == false && older.isEmpty() == false && newer != older, "lesson ids" );
	ClassroomAttendance attendance;
	attendance.lessonId = older;
	attendance.className = QStringLiteral("Class A");
	attendance.seat = QStringLiteral("1");
	attendance.student = QStringLiteral("Zhang");
	attendance.status = ClassroomAttendance::Late;
	attendance.reason = QStringLiteral("迟到 5 分钟");
	check( book.setAttendance( attendance ) && book.attendanceText( older, QStringLiteral("1") ) ==
			   QStringLiteral("迟到"), "attendance status is persisted" );
	check( book.levelName( 59 ) == QStringLiteral("重度危险玩家") &&
		   book.levelName( 100 ) == QStringLiteral("积极玩家") &&
		   book.levelName( 500 ) == QStringLiteral("超神玩家"),
		   "default level thresholds match the classroom rules" );

	ClassroomScoreEntry late;
	late.lessonId = older;
	late.seat = QStringLiteral("1");
	late.student = QStringLiteral("Zhang");
	late.delta = -5;
	late.label = QStringLiteral("late -5");
	book.addEntry( late );

	ClassroomScoreEntry good;
	good.lessonId = newer;
	good.seat = QStringLiteral("1");
	good.student = QStringLiteral("Zhang");
	good.delta = 3;
	good.label = QStringLiteral("good +3");
	book.addEntry( good );

	check( book.total( QStringLiteral("1"), older ) == 95, "total up to earlier lesson" );
	check( book.total( QStringLiteral("1"), newer ) == 98, "total up to later lesson" );
	check( book.total( QStringLiteral("1") ) == 98, "final total" );
	check( book.entriesForCell( older, QStringLiteral("1") ).size() == 1, "one cell per lesson" );
	check( book.studentHtml( QStringLiteral("1"), QStringLiteral("Zhang") ).
		   contains( QStringLiteral("Scoring rules") ) == false, "unpublished rules are hidden" );

	ClassroomScoreRules published = rules;
	published.published = true;
	published.html = QStringLiteral("<b>Rule</b>");
	book.setRules( published );
	check( book.studentHtml( QStringLiteral("1"), QStringLiteral("Zhang") ).
		   contains( QStringLiteral("<b>Rule</b>") ), "published rules are visible" );

	const auto words = ClassroomWordCloud::tokenize(
		{ QStringLiteral("画图软件 画图软件 画图软件 练习"), QStringLiteral("WPS表格 WPS表格") },
		{ QStringLiteral("画图软件"), QStringLiteral("WPS表格") }, 60 );
	check( words.isEmpty() == false, "word cloud produces tokens" );
	bool hasLongWord = false;
	for( const auto& word : words )
	{
		if( word.text.size() >= 4 ) { hasLongWord = true; }
	}
	check( hasLongWord, "word cloud keeps specific long terms" );
	check( ClassroomWordCloud::toSvg( words ).startsWith( QByteArrayLiteral("<?xml") ), "svg output" );
	check( ClassroomWordCloud::toPixmap( words, 320, 200 ).isNull() == false, "pixmap output" );

	ClassroomRoster roster( directory.path() );
	ClassroomClass classItem;
	classItem.name = QStringLiteral("Class A");
	ClassroomStudent student;
	student.seat = QStringLiteral("1");
	student.name = QStringLiteral("Zhang");
	student.ip = QStringLiteral("192.168.1.10");
	classItem.students.append( student );
	ClassroomStudent secondStudent;
	secondStudent.seat = QStringLiteral("2");
	secondStudent.name = QStringLiteral("Li");
	secondStudent.ip = QStringLiteral("192.168.1.11");
	classItem.students.append( secondStudent );
	roster.classes().append( classItem );
	roster.save();

	ClassroomTermReport::Options options;
	options.className = QStringLiteral("Class A");
	options.attendanceColumn = true;
	const auto xml = ClassroomTermReport::toSpreadsheetMl( roster, book, options );
	check( xml.contains( QByteArrayLiteral("<?xml") ), "report xml declaration" );
	check( xml.contains( QByteArrayLiteral("</Workbook>") ), "report workbook closed" );
	check( xml.contains( QByteArrayLiteral("98") ), "report contains current total" );
	check( ClassroomTermReport::toCsv( roster, book, options ).startsWith( QByteArrayLiteral("\xEF\xBB\xBF") ),
		   "csv has UTF-8 BOM" );
	check( xml.contains( QByteArrayLiteral("积分明细") ),
		   "term report contains the per-entry detail worksheet" );
	check( xml.contains( QByteArrayLiteral("考勤明细") ) && xml.contains( QByteArrayLiteral("迟到 5 分钟") ),
		   "term report contains attendance status and custom reason" );
	check( ClassroomTermReport::toDetailsCsv( book, options ).contains( QByteArrayLiteral("原因") ),
		   "detail csv carries the reason column" );
	check( static_cast<int>( Classroom::Command::StartCheckIn ) == 37 &&
		   static_cast<int>( Classroom::Command::CheckInReply ) == 38 &&
		   static_cast<int>( Classroom::Argument::CheckInName ) == 49,
		   "wire protocol enum values stay stable" );
	check( static_cast<int>( Classroom::Command::ShopState ) == 52 &&
		   static_cast<int>( Classroom::Command::ShopQuery ) == 53 &&
		   static_cast<int>( Classroom::Command::ShopRedeem ) == 54 &&
		   static_cast<int>( Classroom::Command::ShopBid ) == 55 &&
		   static_cast<int>( Classroom::Command::ShopReply ) == 56 &&
		   static_cast<int>( Classroom::Command::ShopImageQuery ) == 57 &&
		   static_cast<int>( Classroom::Command::ShopImageReply ) == 58 &&
		   static_cast<int>( Classroom::Argument::ShopJson ) == 57 &&
		   static_cast<int>( Classroom::Argument::ProductId ) == 58 &&
		   static_cast<int>( Classroom::Argument::AuctionId ) == 59 &&
		   static_cast<int>( Classroom::Argument::Amount ) == 60 &&
		   static_cast<int>( Classroom::Argument::ShopAction ) == 61 &&
		   static_cast<int>( Classroom::Argument::ShopImageId ) == 62 &&
		   static_cast<int>( Classroom::Argument::ShopImageData ) == 63,
		   "shop protocol values are append-only" );
	check( static_cast<int>( Classroom::Command::DeployPackageBegin ) == 59 &&
		   static_cast<int>( Classroom::Command::DeployPackageChunk ) == 60 &&
		   static_cast<int>( Classroom::Command::DeployPackageCommit ) == 61 &&
		   static_cast<int>( Classroom::Command::DeployPackageResult ) == 62 &&
		   static_cast<int>( Classroom::Argument::DeploymentId ) == 64 &&
		   static_cast<int>( Classroom::Argument::ManifestJson ) == 65 &&
		   static_cast<int>( Classroom::Argument::RelativePath ) == 66 &&
		   static_cast<int>( Classroom::Argument::FileSha256 ) == 67 &&
		   static_cast<int>( Classroom::Argument::FileOffset ) == 68 &&
		   static_cast<int>( Classroom::Argument::ShortcutName ) == 69 &&
		   static_cast<int>( Classroom::Argument::ShortcutTarget ) == 70 &&
		   static_cast<int>( Classroom::Argument::DeployPhase ) == 71,
		   "folder deployment protocol values are append-only" );

	// ------------------------------------------------------------- shop book
	// Seat 1 enters this section with 98 points and seat 2 with the base 100.
	ClassroomShopBook shop( directory.path() );
	check( shop.load( QStringLiteral("Class A") ), "load fresh shop book" );
	check( shop.bookScore( QStringLiteral("1"), QStringLiteral("Zhang"), 12, QStringLiteral("test award") ),
		  "shop test starts above the protected score" );
	check( shop.isOpen() && shop.revision() == 0, "fresh shop book" );

	ClassroomShopProduct pen;
	pen.name = QStringLiteral("圆珠笔");
	pen.description = QStringLiteral( "一支普通的圆珠笔" );
	pen.price = 10;
	pen.stock = 2;
	pen.image = QByteArray( "\x89PNG fake", 10 );
	check( shop.addProduct( &pen ) && pen.id.isEmpty() == false, "shop add product" );

	auto tooExpensive = pen;
	tooExpensive.price = ClassroomShopBook::MaxPrice + 1;
	check( shop.addProduct( &tooExpensive ) && shop.products().constLast().price <= ClassroomShopBook::MaxPrice,
		   "product price is clamped" );

	check( shop.setProductListed( pen.id, false ) && shop.product( pen.id )->listed == false,
		  "product can be taken off the shelf" );
	const auto hiddenProducts = shop.catalogJson().value( QStringLiteral("products") ).toArray();
	check( hiddenProducts.size() == 1 && hiddenProducts.first().toObject().value( QStringLiteral("id") ).toString() != pen.id,
		  "unlisted products are hidden from students" );
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), 110 ).contains( QStringLiteral("下架") ),
		  "unlisted product cannot be redeemed" );
	check( shop.setProductListed( pen.id, true ), "product can be listed again" );

	// insufficient balance must be rejected without touching anything
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), 5 ).
		   contains( QStringLiteral("积分不足") ), "redeem with low balance is rejected" );

	check( ClassroomShopBook::spendableScore( 79 ) == 0 &&
		   ClassroomShopBook::spendableScore( 80 ) == 0 &&
		   ClassroomShopBook::spendableScore( 100 ) == 20 &&
		   ClassroomShopBook::spendableScore( 110 ) == 30,
		   "shop can only spend points above the protected 80" );
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), shop.balance( QStringLiteral("1") ) ).isEmpty(),
		   "redeem with enough spendable balance succeeds" );
	check( shop.product( pen.id )->stock == 1, "stock decremented once" );
	check( shop.redemptions().size() == 1 &&
		   shop.redemptions().first().kind == ClassroomShopRedemption::Purchase &&
		   shop.redemptions().first().status == ClassroomShopRedemption::Pending,
		   "redemption queued as pending purchase" );
	check( shop.balance( QStringLiteral("1") ) == 100, "redeem booked -10 points (110-10)" );

	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), shop.balance( QStringLiteral("1") ) ).isEmpty(),
		   "second redeem succeeds" );
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), shop.balance( QStringLiteral("1") ) ).
		   contains( QStringLiteral("兑完") ), "redeem without stock is rejected" );

	shop.setOpen( false );
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), 50 ).
		   contains( QStringLiteral("关闭") ), "closed shop rejects redeeming" );
	shop.setOpen( true );

	const auto auctionId = shop.startAuction( QStringLiteral("课外读物"), 10, 2 );
	check( auctionId.isEmpty() == false, "auction started" );
	check( shop.placeBid( auctionId, 5, QStringLiteral("2"), QStringLiteral("Li"), 100 ).
		   contains( QStringLiteral("至少") ), "bid below start price is rejected" );
	check( shop.placeBid( auctionId, 10, QStringLiteral("2"), QStringLiteral("Li"), 5 ).
		   contains( QStringLiteral("积分不足") ), "bid above balance is rejected" );
	check( shop.placeBid( auctionId, 10, QStringLiteral("2"), QStringLiteral("Li"), 100 ).isEmpty(),
		   "first accepted bid at start price" );
	check( shop.placeBid( auctionId, 11, QStringLiteral("1"), QStringLiteral("Zhang"), 100 ).
		   contains( QStringLiteral("至少") ), "bid below increment is rejected" );
	check( shop.placeBid( auctionId, 12, QStringLiteral("1"), QStringLiteral("Zhang"), 100 ).isEmpty(),
		   "bid respects the increment" );
	check( shop.placeBid( auctionId, 14, QStringLiteral("2"), QStringLiteral("Li"), 100 ).isEmpty(),
		   "higher bid wins" );

	const auto closed = shop.closeAuction( auctionId );
	check( closed.found && closed.sold && closed.price == 14 &&
		   closed.winnerSeat == QStringLiteral("2") && closed.winnerStudent == QStringLiteral("Li"),
		   "highest bid wins the auction" );
	check( shop.balance( QStringLiteral("2") ) == 86, "win booked -14 points" );
	check( shop.redemptions().size() == 3 &&
		   shop.redemptions().last().kind == ClassroomShopRedemption::AuctionWin,
		   "auction win queued for delivery" );

	const auto emptyAuction = shop.startAuction( QStringLiteral("流拍品"), 10, 1 );
	check( emptyAuction.isEmpty() == false, "second auction started" );
	const auto unsold = shop.closeAuction( emptyAuction );
	check( unsold.found && unsold.sold == false, "auction without bids closes unsold" );
	check( shop.balance( QStringLiteral("2") ) == 86, "unsold auction charges nobody" );

	const auto underfundedAuction = shop.startAuction( QStringLiteral("余额不足测试" ), 20, 1 );
	check( shop.bookScore( QStringLiteral("2"), QStringLiteral("Li"), 20, QStringLiteral("test award") ) &&
		   shop.placeBid( underfundedAuction, 20, QStringLiteral("2"), QStringLiteral("Li"), shop.balance( QStringLiteral("2") ) ).isEmpty(),
		   "underfunded test bid accepted while balance is sufficient" );
	check( shop.bookScore( QStringLiteral("2"), QStringLiteral("Li"), -80, QStringLiteral("test spend") ),
		   "balance can change after a bid" );
	const auto underfunded = shop.closeAuction( underfundedAuction );
	check( underfunded.error.isEmpty() == false && underfunded.sold == false &&
		   shop.auction( underfundedAuction )->open,
		   "auction stays open when winner can no longer pay" );
	check( shop.balance( QStringLiteral("2") ) == 26, "failed auction close does not charge again" );
	check( shop.removeAuction( underfundedAuction ), "underfunded auction can be cancelled" );

	const auto switchedAuction = shop.startAuction( QStringLiteral("换座测试" ), 10, 1 );
	check( shop.bookScore( QStringLiteral("2"), QStringLiteral("Li"), 80, QStringLiteral("test award") ) &&
		   shop.placeBid( switchedAuction, 10, QStringLiteral("2"), QStringLiteral("Li"), shop.balance( QStringLiteral("2") ) ).isEmpty(),
		   "seat-switch test bid accepted" );
	auto* secondRosterStudent = roster.findStudent( QStringLiteral("Class A"), QStringLiteral("2") );
	check( secondRosterStudent != nullptr, "seat-switch roster student found" );
	if( secondRosterStudent )
	{
		const auto originalName = secondRosterStudent->name;
		secondRosterStudent->name = QStringLiteral("Changed");
		roster.save();
		const auto switched = shop.closeAuction( switchedAuction );
		check( switched.error.isEmpty() == false && switched.sold == false &&
			   shop.auction( switchedAuction )->open,
			   "auction cannot charge a different student after a seat switch" );
		secondRosterStudent->name = originalName;
		roster.save();
	}
	check( shop.removeAuction( switchedAuction ), "seat-switch auction can be cancelled" );

	// reject the shop purchase from above: refund + restock
	// seat 1 ledger: 98 + 12 - 10 - 10 + 10(refund) = 100
	const auto purchaseId = shop.redemptions().first().id;
	const auto deliveredPurchaseId = shop.redemptions().at( 1 ).id;
	check( shop.setRedemptionStatus( deliveredPurchaseId, ClassroomShopRedemption::Delivered ),
		   "purchase can be marked delivered" );
	check( shop.setRedemptionStatus( deliveredPurchaseId, ClassroomShopRedemption::Rejected ) == false &&
		   shop.setRedemptionStatus( deliveredPurchaseId, 99 ) == false,
		   "delivered or invalid redemption states cannot move backwards" );
	check( shop.balance( QStringLiteral("1") ) == 90, "invalid status changes do not refund" );
	auto* firstRosterStudent = roster.findStudent( QStringLiteral("Class A"), QStringLiteral("1") );
	check( firstRosterStudent != nullptr, "refund roster student found" );
	if( firstRosterStudent )
	{
		const auto originalName = firstRosterStudent->name;
		firstRosterStudent->name = QStringLiteral("Changed");
		roster.save();
		check( shop.setRedemptionStatus( purchaseId, ClassroomShopRedemption::Rejected ) == false &&
			   shop.balance( QStringLiteral("1") ) == 90,
			   "refund cannot move to a different student after a seat switch" );
		firstRosterStudent->name = originalName;
		roster.save();
	}
	check( shop.setRedemptionStatus( purchaseId, ClassroomShopRedemption::Rejected ),
		   "purchase rejection accepted" );
	check( shop.balance( QStringLiteral("1") ) == 100, "refund booked +10 points" );
	check( shop.product( pen.id ) && shop.product( pen.id )->stock == 1,
		   "rejected purchase restocks one unit" );

	// every shop charge is a real score-book entry (-10, -10, +10)
	book.load( QStringLiteral("Class A") );
	int shopSpend = 0;
	bool hasShopLabel = false;
	for( const auto& entry : book.entriesForStudent( QStringLiteral("1") ) )
	{
		// only the shop's own entries count; the earlier scoring checks add -5/+3
		if( entry.label.contains( QStringLiteral("圆珠笔") ) )
		{
			shopSpend += entry.delta;
			hasShopLabel = hasShopLabel || entry.label.contains( QStringLiteral("兑换") );
		}
	}
	check( shopSpend == -10, "score book holds the net shop spend" );
	check( hasShopLabel, "score entry carries the shop label" );

	const auto idempotentBalance = shop.balance( QStringLiteral("1") );
	const auto idempotentStock = shop.product( pen.id )->stock;
	const auto idempotentRedemptions = shop.redemptions().size();
	check( shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), idempotentBalance,
					   QStringLiteral("stable-operation-1") ).isEmpty() &&
		   shop.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), shop.balance( QStringLiteral("1") ),
					   QStringLiteral("stable-operation-1") ).isEmpty() &&
		   shop.product( pen.id )->stock == idempotentStock - 1 &&
		   shop.redemptions().size() == idempotentRedemptions + 1 &&
		   shop.balance( QStringLiteral("1") ) == idempotentBalance - pen.price,
		   "a repeated stable operation id charges and restocks exactly once" );
	check( shop.redeem( pen.id, QStringLiteral("2"), QStringLiteral("Li"), 100,
					   QStringLiteral("stable-operation-1") ).isEmpty() == false &&
		   shop.product( pen.id )->stock == idempotentStock - 1,
		   "a stable operation id cannot be rebound to another student" );

	// persistence across a reload
	ClassroomShopBook reloaded( directory.path() );
	check( reloaded.load( QStringLiteral("Class A") ), "shop book reload" );
	check( reloaded.products().size() == shop.products().size() &&
		   reloaded.auctions().size() == shop.auctions().size() &&
		   reloaded.redemptions().size() == shop.redemptions().size() &&
		   reloaded.revision() == shop.revision(),
		   "shop book survives a reload" );
	check( reloaded.products().first().image.isEmpty() == false, "product picture persisted" );

	const auto catalog = reloaded.catalogJson();
	check( catalog.value( QStringLiteral("products") ).toArray().size() == reloaded.products().size() &&
		   catalog.value( QStringLiteral("auctions") ).toArray().size() == reloaded.auctions().size(),
		   "catalog matches the book" );
	check( catalog.value( QStringLiteral("products") ).toArray().first().toObject().
		   value( QStringLiteral("image") ).toString().isEmpty(),
		   "catalog omits pictures by default" );
	check( reloaded.productImage( pen.id ) == pen.image &&
		   reloaded.product( pen.id )->imageId == pen.imageId,
		   "product picture and image id persisted" );

	const auto catalogWithImages = reloaded.catalogJson( true );
	check( catalogWithImages.value( QStringLiteral("products") ).toArray().first().toObject().
		   value( QStringLiteral("image") ).toString().isEmpty() == false,
		   "explicit catalog payload can carry the picture" );
	check( catalogWithImages.value( QStringLiteral("products") ).toArray().first().toObject().
		   value( QStringLiteral("stock") ).toInt() == idempotentStock - 1,
		   "catalog carries live stock" );
	check( reloaded.redeem( pen.id, QStringLiteral("1"), QStringLiteral("Zhang"), reloaded.balance( QStringLiteral("1") ),
							QStringLiteral("stable-operation-1") ).isEmpty() &&
		   reloaded.product( pen.id )->stock == idempotentStock - 1 &&
		   reloaded.redemptions().size() == idempotentRedemptions + 1,
		   "completed operation ids remain idempotent after reload" );

	QImage sourceImage( 320, 240, QImage::Format_RGB32 );
	sourceImage.fill( Qt::red );
	const auto compressed = ClassroomShopBook::catalogImage( sourceImage );
	check( compressed.isEmpty() == false && compressed.size() <= ClassroomShopBook::MaxImageBytes &&
		   QImage::fromData( compressed ).isNull() == false,
		   "uploaded image is compressed to a valid bounded PNG" );

	ClassroomShopBook broken( directory.path() );
	broken.load( QStringLiteral("Broken") );
	QFile brokenFile( broken.filePath() );
	check( QDir().mkpath( ClassroomShopBook::directoryPath( directory.path() ) ) &&
		   brokenFile.open( QIODevice::WriteOnly ) &&
		   brokenFile.write( "{broken" ) == 7,
		   "corrupt shop fixture written" );
	brokenFile.close();
	check( broken.load( QStringLiteral("Broken") ) == false && broken.isLoaded() &&
		   broken.products().isEmpty() && broken.auctions().isEmpty() && broken.redemptions().isEmpty(),
		   "corrupt shop data is rejected without replacing the last valid state" );
	check( brokenFile.open( QIODevice::ReadOnly ) && brokenFile.readAll() == QByteArrayLiteral("{broken"),
		   "rejected load never overwrites corrupt data" );
	brokenFile.close();

	ClassroomShopBook emptyFile( directory.path() );
	emptyFile.load( QStringLiteral("Empty") );
	QFile emptyFixture( emptyFile.filePath() );
	check( emptyFixture.open( QIODevice::WriteOnly ) && emptyFixture.resize( 0 ),
		   "empty shop fixture written" );
	emptyFixture.close();
	check( emptyFile.load( QStringLiteral("Empty") ) == false,
		   "empty shop data is rejected instead of silently reset" );

	ClassroomShopBook history( directory.path() );
	check( history.load( QStringLiteral("History") ), "history shop book loads" );
	bool historyOk = true;
	for( int index = 0; index < ClassroomShopBook::MaxStoredAuctions; ++index )
	{
		const auto auctionId = history.startAuction( QStringLiteral("历史 %1" ).arg( index ), 1, 1 );
		if( auctionId.isEmpty() || history.closeAuction( auctionId ).found == false ) { historyOk = false; break; }
	}
	check( historyOk && history.auctions().size() == ClassroomShopBook::MaxStoredAuctions,
		   "closed auction history reaches its bounded capacity" );
	const auto recycledId = history.startAuction( QStringLiteral("回收测试" ), 1, 1 );
	check( recycledId.isEmpty() == false && history.auctions().size() == ClassroomShopBook::MaxStoredAuctions &&
		   history.auction( recycledId )->open,
		   "new auction recycles one closed history record" );

	// ------------------------------------------------------- roster hardening
	// Student-controlled text must not turn into a formula in Excel.
	ClassroomEvent formulaEvent;
	formulaEvent.timestamp = QDateTime( QDate( 2026, 11, 4 ), QTime( 9, 0 ) );
	formulaEvent.student = QStringLiteral("=HYPERLINK(\"http://evil\")");
	formulaEvent.detail = QStringLiteral("  +1|calc");
	formulaEvent.className = QStringLiteral("Class A");
	QString csvError;
	check( ClassroomLogStore::exportCsv( QDir( directory.path() ).filePath( QStringLiteral("events.csv") ),
										 { formulaEvent }, &csvError ) &&
		   csvError.isEmpty(),
		   "event log exports to csv" );
	QFile exportedCsv( QDir( directory.path() ).filePath( QStringLiteral("events.csv") ) );
	check( exportedCsv.open( QIODevice::ReadOnly ), "exported csv can be reopened" );
	const auto exportedText = QString::fromUtf8( exportedCsv.readAll() );
	exportedCsv.close();
	check( exportedText.contains( QStringLiteral("\"'=HYPERLINK(\"\"http://evil\"\")\"") ) &&
		   exportedText.contains( QStringLiteral("\"'  +1|calc\"") ),
		   "csv export neutralises formula leads in student text" );

	ClassroomRoster formulaRoster( directory.path() );
	ClassroomClass formulaClass;
	formulaClass.name = QStringLiteral("Class Z");
	ClassroomStudent formulaStudent;
	formulaStudent.seat = QStringLiteral("7");
	formulaStudent.name = QStringLiteral("@SUM(1)");
	formulaStudent.ip = QStringLiteral("192.168.1.70");
	formulaClass.students.append( formulaStudent );
	formulaRoster.classes().append( formulaClass );
	ClassroomTermReport::Options formulaOptions;
	formulaOptions.className = QStringLiteral("Class Z");
	check( ClassroomTermReport::toCsv( formulaRoster, book, formulaOptions ).
		   contains( QByteArrayLiteral("\"'@SUM(1)\"") ),
		   "term report csv neutralises formula leads in names" );
	ClassroomScoreEntry formulaEntry;
	formulaEntry.lessonId = older;
	formulaEntry.seat = QStringLiteral("3");
	formulaEntry.student = QStringLiteral("Formula");
	formulaEntry.delta = -1;
	formulaEntry.label = QStringLiteral("=cmd");
	book.addEntry( formulaEntry );
	check( ClassroomTermReport::toDetailsCsv( book, options ).contains( QByteArrayLiteral("\"'=cmd\"") ),
		   "term detail csv neutralises formula leads in reasons" );

	// Imports are staged: a rejected row leaves the roster exactly as it was.
	QTemporaryDir importDirectory;
	check( importDirectory.isValid(), "import temporary directory" );
	ClassroomRoster importRoster( importDirectory.path() );
	ClassroomImportReport importReport;
	check( importRoster.importTable( QStringLiteral("1;Bad;192.168.1\n"), {}, &importReport ) == false &&
		   importRoster.classes().isEmpty() && importReport.problems.size() == 1,
		   "invalid IP addresses are refused and leave no half-applied class" );
	importReport = ClassroomImportReport{};
	check( importRoster.importTable(
			   QStringLiteral("班级;座位号;姓名;IP地址\n"
							  "3年级1班;1;\"张\n三\";192.168.1.101\n"
							  "3年级1班;2;李四;192.168.1.102\n"
							  "3年级1班;1;张三;192.168.1.102\n"), {}, &importReport ) &&
		   importRoster.classes().size() == 1 &&
		   importRoster.findStudent( QStringLiteral("3年级1班"), QStringLiteral("1") )->name == QStringLiteral("张\n三") &&
		   importRoster.findStudent( QStringLiteral("3年级1班"), QStringLiteral("1") )->ip == QStringLiteral("192.168.1.101") &&
		   importReport.students == 2 && importReport.overwritten == 0 && importReport.ipConflicts == 1,
		   "quoted line breaks are kept and a seat cannot take another seat's IP" );
	const auto rosterJson = importRoster.toJson();
	check( importRoster.importTable( QStringLiteral("姓名\n=只有人名\n"), {}, &importReport ) == false &&
		   importRoster.toJson() == rosterJson,
		   "an import that changes nothing does not rewrite the roster" );

	QString manyRows = QStringLiteral("座位号;姓名\n");
	for( int index = 0; index < 30; ++index ) { manyRows += QStringLiteral("%1;学生%2\n").arg( index ).arg( index ); }
	importReport = ClassroomImportReport{};
	check( importRoster.importTable( manyRows, QStringLiteral("大班"), &importReport ) &&
		   importReport.problems.isEmpty(),
		   "a regular class imports without warnings" );

	// Storage keys must stay injective: two different class names never share
	// one score or shop file, even when both sanitize to the same text.
	check( ClassroomLogStore::storageKey( QStringLiteral("A:B") ) !=
			   ClassroomLogStore::storageKey( QStringLiteral("A-B") ) &&
			   ClassroomLogStore::storageKey( QStringLiteral("A:B") ) !=
			   ClassroomLogStore::storageKey( QStringLiteral("A_B") ),
		   "storage keys of different class names stay distinct" );
	check( ClassroomLogStore::storageKey( QStringLiteral("3年级1班") ) ==
			   QStringLiteral("3年级1班") &&
		   ClassroomLogStore::sanitize( QStringLiteral("Class A") ) == QStringLiteral("Class_A"),
		   "clean names keep their key and legacy log paths are unchanged" );
	check( ClassroomLogStore::sanitize( QStringLiteral("") ) == QStringLiteral("unknown"),
		   "empty names fall back to unknown" );

	// A book stored under the old lossy key is migrated on first load.
	const QString legacyName = ClassroomLogStore::sanitize( QStringLiteral("Class A") ) +
							   QStringLiteral(".json");
	const QString modernName = ClassroomLogStore::storageKey( QStringLiteral("Class A") ) +
							   QStringLiteral(".json");
	QTemporaryDir migrationDirectory;
	check( migrationDirectory.isValid(), "migration temporary directory" );
	const QString scoresDir = QDir( migrationDirectory.path() ).filePath( QStringLiteral("scores") );
	check( QDir().mkpath( scoresDir ), "scores directory created" );
	{
		QFile legacyBook( QDir( scoresDir ).filePath( legacyName ) );
		const QByteArray legacyBody( "{\"rules\":{},\"lessons\":[],\"entries\":[],\"attendance\":[]}" );
		check( legacyBook.open( QIODevice::WriteOnly ) && legacyBook.write( legacyBody ) == legacyBody.size(),
			   "legacy score book fixture written" );
		legacyBook.close();
	}
	ClassroomScoreBook migratedBook( migrationDirectory.path() );
	migratedBook.load( QStringLiteral("Class A") );
	check( QFile( QDir( scoresDir ).filePath( modernName ) ).exists() &&
		   QFile( QDir( scoresDir ).filePath( legacyName ) ).exists() == false,
		   "legacy score book migrates to the injective key on load" );

	// The staged roster is written in one step and reads back unchanged.
	const auto stagedJson = importRoster.toJson();
	check( importRoster.save(), "roster saves atomically" );
	ClassroomRoster reloadedRoster( importDirectory.path() );
	check( reloadedRoster.toJson() == stagedJson &&
		   reloadedRoster.findStudent( QStringLiteral("大班"), QStringLiteral("29") ) != nullptr,
		   "roster reload matches what was saved" );

	// ------------------------------------------- transactional JSON persistence
	// Rejected score books must preserve both the last good in-memory state and
	// the exact bytes on disk, including truncated and wrong-shaped documents.
	QTemporaryDir scoreDirectory;
	check( scoreDirectory.isValid(), "score persistence temporary directory" );
	ClassroomScoreBook stableBook( scoreDirectory.path() );
	check( stableBook.load( QStringLiteral("Stable") ), "missing score book retains fresh-load semantics" );
	check( stableBook.load( QStringLiteral("Stable") ), "an unsaved fresh score book can be reloaded before its first save" );
	ClassroomScoreRules stableRules;
	stableRules.baseScore = 87;
	stableRules.html = QStringLiteral("<b>Stable rules</b>");
	check( stableBook.setRules( stableRules ), "stable score rules saved" );
	const auto stableLesson = stableBook.ensureLesson( QDate( 2026, 11, 5 ), QStringLiteral("Persistence") );
	ClassroomScoreEntry stableEntry;
	stableEntry.lessonId = stableLesson;
	stableEntry.seat = QStringLiteral("1");
	stableEntry.student = QStringLiteral("Stable student");
	stableEntry.delta = 2;
	stableEntry.label = QStringLiteral("stable entry");
	check( stableBook.addEntry( stableEntry ), "stable score entry saved" );
	ClassroomAttendance stableAttendance;
	stableAttendance.lessonId = stableLesson;
	stableAttendance.seat = QStringLiteral("1");
	stableAttendance.student = QStringLiteral("Stable student");
	stableAttendance.status = ClassroomAttendance::Present;
	check( stableBook.setAttendance( stableAttendance ), "stable attendance saved" );
	const auto stableSnapshot = QJsonDocument( stableBook.syncSnapshot() ).toJson( QJsonDocument::Compact );

	const auto readFixture = []( const QString& path ) {
		QFile file( path );
		if( file.open( QIODevice::ReadOnly ) == false ) { return QByteArray{}; }
		return file.readAll();
	};
	const auto writeFixture = []( const QString& path, const QByteArray& bytes ) {
		QFile file( path );
		if( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) == false ) { return false; }
		if( file.write( bytes ) != bytes.size() ) { return false; }
		return file.flush();
	};
	const auto validScoreBytes = readFixture( stableBook.filePath( QStringLiteral("Stable") ) );
	check( validScoreBytes.isEmpty() == false, "valid score fixture exists" );
	const QVector<QByteArray> invalidScoreFixtures{
		QByteArrayLiteral("{\"rules\":"),
		QByteArrayLiteral("[]"),
		QByteArrayLiteral("{}"),
		QByteArrayLiteral("{\"rules\":{},\"lessons\":[],\"entries\":[]}"),
		QByteArrayLiteral("{\"lessons\":[],\"entries\":[],\"attendance\":[{}]}"),
		QByteArrayLiteral("{\"rules\":{},\"entries\":{}}"),
		QByteArrayLiteral("{\"lessons\":[1]}"),
		QByteArrayLiteral("{\"entries\":[{\"delta\":\"bad\"}]}"),
		QByteArrayLiteral("{\"attendance\":[{\"status\":\"present\"}]}"),
		QByteArrayLiteral("{\"rules\":{\"quickActions\":[[\"Good\",\"bad\"]]}}")
	};
	bool scoreCorruptionPreservedState = true;
	bool scoreCorruptionPreservedBytes = true;
	for( const auto& fixture : invalidScoreFixtures )
	{
		if( writeFixture( stableBook.filePath( QStringLiteral("Stable") ), fixture ) == false ||
			stableBook.load( QStringLiteral("Stable") ) != false ||
			stableBook.className() != QStringLiteral("Stable") ||
			QJsonDocument( stableBook.syncSnapshot() ).toJson( QJsonDocument::Compact ) != stableSnapshot ||
			readFixture( stableBook.filePath( QStringLiteral("Stable") ) ) != fixture )
		{
			scoreCorruptionPreservedState = false;
			scoreCorruptionPreservedBytes = false;
		}
	}
	check( scoreCorruptionPreservedState, "corrupt score JSON preserves prior lessons, entries, attendance and rules" );
	check( scoreCorruptionPreservedBytes, "rejected score loads never rewrite corrupt bytes" );
	check( writeFixture( stableBook.filePath( QStringLiteral("Stable") ), validScoreBytes ) &&
		   stableBook.load( QStringLiteral("Stable") ) &&
		   QJsonDocument( stableBook.syncSnapshot() ).toJson( QJsonDocument::Compact ) == stableSnapshot,
		   "valid score bytes still load after corruption checks" );
	check( QFile::remove( stableBook.filePath( QStringLiteral("Stable") ) ) &&
		   stableBook.load( QStringLiteral("Stable") ) == false &&
		   QJsonDocument( stableBook.syncSnapshot() ).toJson( QJsonDocument::Compact ) == stableSnapshot,
		   "a disappeared score file never resets an already loaded book" );
	check( writeFixture( stableBook.filePath( QStringLiteral("Stable") ), validScoreBytes ),
		   "score fixture restored after disappearance check" );

	// Roster parsing uses the same all-or-nothing contract for direct JSON and
	// file loads. A valid empty classes array remains an explicit clear.
	QTemporaryDir rosterDirectory;
	check( rosterDirectory.isValid(), "roster persistence temporary directory" );
	ClassroomRoster stableRoster( rosterDirectory.path() );
	ClassroomClass stableClass;
	stableClass.name = QStringLiteral("Stable");
	stableClass.students.append( ClassroomStudent{ QStringLiteral("1"), QStringLiteral("Stable student"),
												  QStringLiteral("192.168.1.101") } );
	stableRoster.classes().append( stableClass );
	check( stableRoster.save(), "stable roster saved" );
	const auto stableRosterJson = stableRoster.toJson();
	const auto validRosterBytes = readFixture( stableRoster.filePath() );
	const QVector<QString> invalidRosterJson{
		QStringLiteral("{broken"),
		QStringLiteral("[]"),
		QStringLiteral("{\"classes\":{}}"),
		QStringLiteral("{\"classes\":[1]}"),
		QStringLiteral("{\"classes\":[{\"name\":\"Bad\",\"students\":{}}]}")
	};
	bool rosterCorruptionPreservedState = true;
	for( const auto& fixture : invalidRosterJson )
	{
		if( stableRoster.fromJson( fixture ) || stableRoster.toJson() != stableRosterJson )
		{
			rosterCorruptionPreservedState = false;
		}
	}
	check( rosterCorruptionPreservedState, "invalid roster JSON and top-level shapes preserve existing classes" );
	const QByteArray truncatedRoster = QByteArrayLiteral("{\"classes\":[");
	check( writeFixture( stableRoster.filePath(), truncatedRoster ) &&
		   stableRoster.load() == false && stableRoster.toJson() == stableRosterJson &&
		   readFixture( stableRoster.filePath() ) == truncatedRoster,
		   "truncated roster load preserves prior classes and original bytes" );
	check( writeFixture( stableRoster.filePath(), validRosterBytes ) && stableRoster.load() &&
		   stableRoster.toJson() == stableRosterJson,
		   "valid roster bytes still load after corruption checks" );

	// Report replacement is atomic on success and reports pre-write failures
	// without modifying an existing filesystem object.
	ClassroomTermReport::Options stableOptions;
	stableOptions.className = QStringLiteral("Stable");
	const auto reportPath = QDir( scoreDirectory.path() ).filePath( QStringLiteral("report.xls") );
	const QByteArray previousReport = QByteArrayLiteral("previous report");
	check( writeFixture( reportPath, previousReport ), "previous report fixture written" );
	QString reportError;
	const auto expectedReport = ClassroomTermReport::toSpreadsheetMl( stableRoster, stableBook, stableOptions );
	check( ClassroomTermReport::exportXls( reportPath, stableRoster, stableBook, stableOptions, &reportError ) &&
		   reportError.isEmpty() && readFixture( reportPath ) == expectedReport,
		   "term report atomically replaces an existing export with the complete workbook" );
	const auto blockerPath = QDir( scoreDirectory.path() ).filePath( QStringLiteral("not-a-directory") );
	check( writeFixture( blockerPath, previousReport ), "report parent blocker fixture written" );
	reportError.clear();
	check( ClassroomTermReport::exportXls( QDir( blockerPath ).filePath( QStringLiteral("report.xls") ),
										  stableRoster, stableBook, stableOptions, &reportError ) == false &&
		   reportError.isEmpty() == false && readFixture( blockerPath ) == previousReport,
		   "failed term report export preserves existing bytes and returns a visible error" );

	// An interrupted purchase rolls its fixed-id charge back and clears the
	// journal without creating a redemption or decrementing stock.
	QTemporaryDir purchaseRecoveryDirectory;
	ClassroomShopBook purchaseRecovery( purchaseRecoveryDirectory.path() );
	check( purchaseRecovery.load( QStringLiteral("PurchaseRecovery") ), "purchase recovery shop loads" );
	ClassroomShopProduct recoveryProduct;
	recoveryProduct.name = QStringLiteral("Recovery item" );
	recoveryProduct.price = 5;
	recoveryProduct.stock = 2;
	check( purchaseRecovery.addProduct( &recoveryProduct ), "purchase recovery product created" );
	ClassroomScoreBook purchaseRecoveryScores( purchaseRecoveryDirectory.path() );
	check( purchaseRecoveryScores.load( QStringLiteral("PurchaseRecovery") ), "purchase recovery score book loads" );
	ClassroomScoreEntry interruptedCharge;
	interruptedCharge.id = QStringLiteral("interrupted-purchase-charge" );
	interruptedCharge.lessonId = purchaseRecoveryScores.ensureLesson( QDate::currentDate(),
		ClassroomShopBook::lessonTopic() );
	interruptedCharge.seat = QStringLiteral("1" );
	interruptedCharge.student = QStringLiteral("Recovery student" );
	interruptedCharge.delta = -5;
	interruptedCharge.label = QStringLiteral("interrupted purchase" );
	check( purchaseRecoveryScores.addEntry( interruptedCharge ), "interrupted purchase charge fixture written" );
	QFile purchaseRecoveryFile( purchaseRecovery.filePath() );
	check( purchaseRecoveryFile.open( QIODevice::ReadOnly ), "purchase recovery journal fixture opened" );
	const auto purchaseRecoveryObject = QJsonDocument::fromJson( purchaseRecoveryFile.readAll() ).object();
	purchaseRecoveryFile.close();
	ClassroomShopRedemption interruptedRedemption;
	interruptedRedemption.id = QStringLiteral("interrupted-purchase" );
	interruptedRedemption.productId = recoveryProduct.id;
	interruptedRedemption.productName = recoveryProduct.name;
	interruptedRedemption.seat = QStringLiteral("1" );
	interruptedRedemption.student = QStringLiteral("Recovery student" );
	interruptedRedemption.price = 5;
	interruptedRedemption.timestamp = QDateTime::currentDateTime();
	auto purchaseJournal = purchaseRecoveryObject;
	purchaseJournal.insert( QStringLiteral("pendingOperation"), QJsonObject{
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"), QStringLiteral("interrupted-operation") },
		{ QStringLiteral("kind"), QStringLiteral("purchase") },
		{ QStringLiteral("scoreEntryId"), interruptedCharge.id },
		{ QStringLiteral("seat"), interruptedRedemption.seat },
		{ QStringLiteral("student"), interruptedRedemption.student },
		{ QStringLiteral("delta"), -5 },
		{ QStringLiteral("redemption"), interruptedRedemption.toStorage() } } );
	check( writeFixture( purchaseRecovery.filePath(), QJsonDocument( purchaseJournal ).toJson( QJsonDocument::Compact ) ),
		   "interrupted purchase journal written" );
	ClassroomShopBook recoveredPurchase( purchaseRecoveryDirectory.path() );
	check( recoveredPurchase.load( QStringLiteral("PurchaseRecovery") ) &&
		   recoveredPurchase.hasPendingOperation() &&
		   recoveredPurchase.recoverPendingOperations() && recoveredPurchase.hasPendingOperation() == false &&
		   recoveredPurchase.redemptions().isEmpty() &&
		   recoveredPurchase.product( recoveryProduct.id )->stock == 2 &&
		   recoveredPurchase.balance( QStringLiteral("1") ) == 100,
		   "restart recovery rolls an uncommitted purchase charge back exactly once" );
	purchaseRecoveryScores.load( QStringLiteral("PurchaseRecovery") );
	check( std::none_of( purchaseRecoveryScores.entries().constBegin(), purchaseRecoveryScores.entries().constEnd(),
		[&]( const ClassroomScoreEntry& entry ) { return entry.id == interruptedCharge.id; } ),
		"rolled-back purchase charge is absent from the score book" );
	check( recoveredPurchase.setOpen( false ), "stable shop state saved before corruption check" );
	check( writeFixture( recoveredPurchase.filePath(), QByteArrayLiteral("{broken") ) &&
		   recoveredPurchase.load( QStringLiteral("PurchaseRecovery") ) == false &&
		   recoveredPurchase.isLoaded() && recoveredPurchase.isOpen() == false &&
		   recoveredPurchase.products().size() == 1,
		   "corrupt shop JSON preserves the last good in-memory catalog" );
	const QByteArray malformedShop = QByteArrayLiteral(
		"{\"open\":true,\"products\":{},\"auctions\":\"bad\",\"redemptions\":42,\"completedOperations\":[]}" );
	check( writeFixture( recoveredPurchase.filePath(), malformedShop ) &&
		   recoveredPurchase.load( QStringLiteral("PurchaseRecovery") ) == false &&
		   recoveredPurchase.isOpen() == false && recoveredPurchase.products().size() == 1 &&
		   readFixture( recoveredPurchase.filePath() ) == malformedShop,
		   "malformed shop field shapes preserve memory and exact disk bytes" );

	QTemporaryDir legacyShopDirectory;
	check( legacyShopDirectory.isValid(), "legacy shop persistence temporary directory" );
	const auto legacyShopPath = QDir( ClassroomShopBook::directoryPath( legacyShopDirectory.path() ) ).
		filePath( ClassroomLogStore::sanitize( QStringLiteral("Legacy Shop") ) + QStringLiteral(".json") );
	check( QDir().mkpath( QFileInfo( legacyShopPath ).absolutePath() ), "legacy shop directory created" );
	const QByteArray legacyShopBytes( "{\"open\":true,\"products\":[],\"auctions\":[],\"redemptions\":[]}" );
	ClassroomShopBook legacyShop( legacyShopDirectory.path() );
	check( writeFixture( legacyShopPath, legacyShopBytes ) && legacyShop.load( QStringLiteral("Legacy Shop") ) &&
		   legacyShop.load( QStringLiteral("Legacy Shop") ) && legacyShop.isLoaded() && legacyShop.isOpen(),
		   "legacy shop JSON without completedOperations still loads" );
	check( QFile::exists( QDir( ClassroomShopBook::directoryPath( legacyShopDirectory.path() ) ).
			filePath( ClassroomLogStore::storageKey( QStringLiteral("Legacy Shop") ) + QStringLiteral(".json") ) ) &&
		   QFile::exists( legacyShopPath ) == false,
		   "legacy shop filename is migrated after successful validation" );
	check( QFile::remove( legacyShop.filePath() ) && legacyShop.load( QStringLiteral("Legacy Shop") ) == false &&
		   legacyShop.isLoaded() && legacyShop.isOpen(),
		   "a disappeared shop file preserves the last good catalog" );
	const auto corruptLegacyShopPath = QDir( ClassroomShopBook::directoryPath( legacyShopDirectory.path() ) ).
		filePath( ClassroomLogStore::sanitize( QStringLiteral("Corrupt Legacy") ) + QStringLiteral(".json") );
	const QByteArray corruptLegacyShopBytes( "{\"open\":true,\"products\":{}}" );
	ClassroomShopBook corruptLegacyShop( legacyShopDirectory.path() );
	check( writeFixture( corruptLegacyShopPath, corruptLegacyShopBytes ) &&
		   corruptLegacyShop.load( QStringLiteral("Corrupt Legacy") ) == false &&
		   readFixture( corruptLegacyShopPath ) == corruptLegacyShopBytes,
		   "corrupt legacy shop JSON is neither accepted nor renamed" );

	// An interrupted rejection rolls forward when its fixed-id refund already
	// reached the score book, restoring stock without duplicating the refund.
	QTemporaryDir rejectionRecoveryDirectory;
	ClassroomShopBook rejectionRecovery( rejectionRecoveryDirectory.path() );
	check( rejectionRecovery.load( QStringLiteral("RejectionRecovery") ), "rejection recovery shop loads" );
	ClassroomShopProduct refundProduct;
	refundProduct.name = QStringLiteral("Refund item" );
	refundProduct.price = 5;
	refundProduct.stock = 1;
	check( rejectionRecovery.addProduct( &refundProduct ) &&
		   rejectionRecovery.redeem( refundProduct.id, QStringLiteral("1"), QStringLiteral("Recovery student" ), 100 ).isEmpty(),
		   "rejection recovery purchase created" );
	const auto refundRedemption = rejectionRecovery.redemptions().constFirst();
	QFile rejectionRecoveryFile( rejectionRecovery.filePath() );
	check( rejectionRecoveryFile.open( QIODevice::ReadOnly ), "rejection recovery journal fixture opened" );
	const auto rejectionRecoveryObject = QJsonDocument::fromJson( rejectionRecoveryFile.readAll() ).object();
	rejectionRecoveryFile.close();
	ClassroomScoreBook rejectionRecoveryScores( rejectionRecoveryDirectory.path() );
	check( rejectionRecoveryScores.load( QStringLiteral("RejectionRecovery") ), "rejection recovery score book loads" );
	ClassroomScoreEntry interruptedRefund;
	interruptedRefund.id = QStringLiteral("interrupted-refund" );
	interruptedRefund.lessonId = rejectionRecoveryScores.ensureLesson( QDate::currentDate(),
		ClassroomShopBook::lessonTopic() );
	interruptedRefund.seat = refundRedemption.seat;
	interruptedRefund.student = refundRedemption.student;
	interruptedRefund.delta = refundRedemption.price;
	interruptedRefund.label = QStringLiteral("interrupted refund" );
	check( rejectionRecoveryScores.addEntry( interruptedRefund ), "interrupted rejection refund fixture written" );
	auto rejectionJournal = rejectionRecoveryObject;
	rejectionJournal.insert( QStringLiteral("pendingOperation"), QJsonObject{
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"), QStringLiteral("interrupted-rejection") },
		{ QStringLiteral("kind"), QStringLiteral("reject") },
		{ QStringLiteral("redemptionId"), refundRedemption.id },
		{ QStringLiteral("scoreEntryId"), interruptedRefund.id },
		{ QStringLiteral("seat"), refundRedemption.seat },
		{ QStringLiteral("student"), refundRedemption.student },
		{ QStringLiteral("delta"), refundRedemption.price },
		{ QStringLiteral("redemption"), refundRedemption.toStorage() } } );
	check( writeFixture( rejectionRecovery.filePath(), QJsonDocument( rejectionJournal ).toJson( QJsonDocument::Compact ) ),
		   "interrupted rejection journal written" );
	ClassroomShopBook recoveredRejection( rejectionRecoveryDirectory.path() );
	check( recoveredRejection.load( QStringLiteral("RejectionRecovery") ) &&
		   recoveredRejection.hasPendingOperation() &&
		   recoveredRejection.recoverPendingOperations() && recoveredRejection.hasPendingOperation() == false &&
		   recoveredRejection.redemption( refundRedemption.id )->status == ClassroomShopRedemption::Rejected &&
		   recoveredRejection.product( refundProduct.id )->stock == 1 &&
		   recoveredRejection.balance( QStringLiteral("1") ) == 100,
		   "restart recovery rolls a committed rejection forward without a duplicate refund" );
	rejectionRecoveryScores.load( QStringLiteral("RejectionRecovery") );
	const int refundCount = std::count_if( rejectionRecoveryScores.entries().constBegin(),
		rejectionRecoveryScores.entries().constEnd(), [&]( const ClassroomScoreEntry& entry ) {
			return entry.id == interruptedRefund.id;
		} );
	check( refundCount == 1, "roll-forward recovery preserves exactly one fixed-id refund" );
	QLockFile shopMutationLock( recoveredRejection.filePath() + QStringLiteral( ".lock" ) );
	check( shopMutationLock.tryLock( 0 ) && recoveredRejection.setOpen( false ) == false,
		   "a competing shop writer cannot mutate a locked book" );
	shopMutationLock.unlock();
	check( recoveredRejection.setOpen( false ), "shop mutation succeeds after the competing lock is released" );

	std::fprintf( stdout, "\n%d failure(s)\n", failures );
	return failures == 0 ? 0 : 1;
}
