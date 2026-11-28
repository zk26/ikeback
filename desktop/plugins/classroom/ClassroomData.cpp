/*
 * ClassroomData.cpp - roster, per-machine event logs and interest analysis
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <algorithm>
#include <cmath>

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
#include <QStringDecoder>
#endif

#include "ClassroomConfiguration.h"
#include "ClassroomData.h"
#include "ClassroomProtocol.h"
#include "ClassroomSecrets.h"


static QStringList classroomSecretKeys()
{
	return { QStringLiteral("sessionKey"), Classroom::SettingKey::aiApiKey(),
			 Classroom::SettingKey::adminKey() };
}


namespace
{

//! Writes the whole document first and then replaces the target in one step, so
//! a full disk or a failing encryption can never leave a truncated file behind.
bool writeFileAtomically( const QString& fileName, const QByteArray& data, QString* error = nullptr )
{
	QDir().mkpath( QFileInfo( fileName ).absolutePath() );
	QSaveFile file( fileName );
#ifndef Q_OS_WIN
	// DPAPI is Windows-only; on development/CI hosts at least keep the file
	// readable by its owner alone.
	file.setPermissions( QFile::ReadOwner | QFile::WriteOwner );
#endif
	if( file.open( QIODevice::WriteOnly ) == false )
	{
		if( error ) { *error = file.errorString(); }
		return false;
	}
	if( file.write( data ) != data.size() )
	{
		file.cancelWriting();
		if( error ) { *error = file.errorString(); }
		return false;
	}
	if( file.commit() ) { return true; }
	if( error ) { *error = file.errorString(); }
	return false;
}

}

static QString csvField( const QString& value )
{
	// Names and free-text reasons are student controlled. A leading =, +, - or @
	// would be evaluated as a formula when the teacher opens the export in
	// Excel, so prefix those cells with an apostrophe.
	auto escaped = value;
	int lead = 0;
	while( lead < escaped.size() && ( escaped.at( lead ).isSpace() || escaped.at( lead ).category() == QChar::Other_Control ) )
	{
		++lead;
	}
	if( lead < escaped.size() && QStringLiteral("=+-@").contains( escaped.at( lead ) ) )
	{
		escaped.prepend( QLatin1Char('\'') );
	}
	escaped.replace( QStringLiteral("\""), QStringLiteral("\"\"") );
	return QStringLiteral("\"%1\"").arg( escaped );
}


static bool writeCsv( const QString& fileName, const QStringList& header,
					  const QVector<QStringList>& rows, QString* error )
{
	QByteArray data( "\xEF\xBB\xBF", 3 );
	QStringList quotedHeader;
	for( const auto& field : header ) { quotedHeader += csvField( field ); }
	data += quotedHeader.join( QLatin1Char(';') ).toUtf8() + QByteArrayLiteral("\n");

	for( const auto& row : rows )
	{
		QStringList quotedRow;
		for( const auto& field : row ) { quotedRow += csvField( field ); }
		data += quotedRow.join( QLatin1Char(';') ).toUtf8() + QByteArrayLiteral("\n");
	}

	return writeFileAtomically( fileName, data, error );
}


QJsonObject ClassroomSettings::fromConfiguration( const ClassroomConfiguration& configuration )
{
	using namespace Classroom;
	return { { SettingKey::enabled(), configuration.enabled() },
			 { SettingKey::dataDirectory(), configuration.dataDirectory() },
			 { SettingKey::exitProtection(), configuration.exitProtection() },
			 { SettingKey::exitAdminUsername(), configuration.exitAdminUsername() },
			 { SettingKey::allowSelfLogin(), configuration.allowSelfLogin() },
			 { SettingKey::aiEnabled(), configuration.aiEnabled() },
			 { SettingKey::aiEndpoint(), configuration.aiEndpoint() },
			 { SettingKey::aiApiKey(), configuration.aiApiKey() },
			 { SettingKey::aiModel(), configuration.aiModel() },
			 { SettingKey::aiSystemPrompt(), configuration.aiSystemPrompt() },
			 { SettingKey::aiTimeout(), configuration.aiTimeout() },
			 { SettingKey::monitoringEnabled(), configuration.monitoringEnabled() },
			 { SettingKey::captureInterval(), configuration.captureInterval() },
			 { SettingKey::captureWindowTitle(), configuration.captureWindowTitle() },
			 { SettingKey::captureProcess(), configuration.captureProcess() },
			 { QStringLiteral("sessionKey"), configuration.sessionKey() },
			 { QStringLiteral("interestCategories"), configuration.interestCategories() },
			 { QStringLiteral("retentionDays"), configuration.retentionDays() },
			 { SettingKey::bridgePort(), configuration.bridgePort() },
			 { SettingKey::bridgeEnabled(), configuration.bridgeEnabled() },
			 { SettingKey::syncEnabled(), configuration.syncEnabled() },
			 { SettingKey::silencePageText(), configuration.silencePageText() },
			 { SettingKey::wordCloudStopWords(), configuration.wordCloudStopWords() } };
}


QJsonObject ClassroomSettings::forStudents( const QJsonObject& object )
{
	using namespace Classroom;

	auto result = object;
	for( const auto& key : classroomSecretKeys() ) { result.remove( key ); }
	// Student machines no longer call the model provider. All AI traffic is
	// proxied by the teacher machine, so endpoint/model/prompt stay there too.
	result.remove( SettingKey::aiEndpoint() );
	result.remove( SettingKey::aiModel() );
	result.remove( SettingKey::aiSystemPrompt() );
	result.remove( SettingKey::aiTimeout() );
	// Students never see the super-admin key itself - only its SHA-256 digest
	// which gates the protected exit and the configurator on student machines.
	const auto adminKey = object.value( SettingKey::adminKey() ).toString();
	result.remove( SettingKey::adminKey() );
	auto adminKeyHash = object.value( SettingKey::adminKeyHash() ).toString();
	if( adminKey.isEmpty() == false )
	{
		adminKeyHash = ClassroomSecrets::adminKeyHash( adminKey );
	}
	if( ClassroomSecrets::adminKeyHashConfigured( adminKeyHash ) )
	{
		result.insert( SettingKey::adminKeyHash(), adminKeyHash );
	}
	else
	{
		result.remove( SettingKey::adminKeyHash() );
	}
	// 下发给学生的肃静文案必须是中文；旧配置中的英文一律回退为「肃静」。
	const auto silenceText = result.value( SettingKey::silencePageText() ).toString().trimmed();
	bool hasChinese = false;
	for( const auto ch : silenceText )
	{
		const auto code = ch.unicode();
		if( code >= 0x4E00 && code <= 0x9FFF ) { hasChinese = true; break; }
	}
	if( hasChinese == false )
	{
		result.insert( SettingKey::silencePageText(), QStringLiteral("肃静") );
	}
	result.insert( QStringLiteral("keyConfigured"),
				   object.value( QStringLiteral("sessionKey") ).toString().isEmpty() == false );
	return result;
}


bool ClassroomSettings::migrateSecrets()
{
	QFile file( m_filePath );
	if( file.open( QFile::ReadOnly ) == false ) { return true; }
	const auto onDisk = QJsonDocument::fromJson( file.readAll() ).object();
	file.close();

	bool hasLegacy = false;
	for( const auto& key : classroomSecretKeys() )
	{
		if( ClassroomSecrets::isLegacyProtected( onDisk.value( key ).toString() ) ) { hasLegacy = true; }
	}
	if( hasLegacy == false ) { return true; }

	// load() has already revealed the old values into m_object. Never rewrite
	// the file if a value failed to decrypt, so a key is not silently erased.
	for( const auto& key : classroomSecretKeys() )
	{
		if( onDisk.contains( key ) &&
			onDisk.value( key ).toString().isEmpty() == false &&
			m_object.value( key ).toString().isEmpty() )
		{
			qWarning( "ikeback: refusing to migrate %s: secret could not be revealed", qPrintable( key ) );
			return false;
		}
	}
	return save();
}


ClassroomSettings::ClassroomSettings( const QString& dataDirectory, const ClassroomConfiguration& configuration ) :
	m_filePath( QDir( dataDirectory ).filePath( QStringLiteral("settings.json") ) ),
	m_object( fromConfiguration( configuration ) )
{
	load();
}


bool ClassroomSettings::load()
{
	QFile file( m_filePath );
	if( file.open( QFile::ReadOnly ) == false ) { return false; }
	const auto document = QJsonDocument::fromJson( file.readAll() );
	if( document.isObject() == false ) { return false; }
	const auto saved = document.object();
	for( auto it = saved.constBegin(); it != saved.constEnd(); ++it )
	{
		m_object.insert( it.key(), it.value() );
	}
	for( const auto& key : classroomSecretKeys() )
	{
		if( m_object.contains( key ) )
		{
			m_object.insert( key, ClassroomSecrets::reveal( m_object.value( key ).toString() ) );
		}
	}
	return true;
}


bool ClassroomSettings::save() const
{
	// Protect every secret before touching the file: a failing encryption then
	// leaves the previous settings.json completely intact.
	QJsonObject onDisk = m_object;
	for( const auto& key : classroomSecretKeys() )
	{
		if( onDisk.contains( key ) )
		{
			const auto plain = onDisk.value( key ).toString();
			const auto protectedValue = ClassroomSecrets::protect( plain );
			if( plain.isEmpty() == false && protectedValue.isEmpty() )
			{
				qWarning( "ikeback: could not protect %s, refusing to store it in the clear", qPrintable( key ) );
				return false;
			}
			onDisk.insert( key, protectedValue );
		}
	}
	return writeFileAtomically( m_filePath, QJsonDocument( onDisk ).toJson( QJsonDocument::Indented ) );
}


QJsonObject ClassroomStudent::toJson() const
{
	return { { QStringLiteral("seat"), seat },
			 { QStringLiteral("name"), name },
			 { QStringLiteral("ip"), ip } };
}


ClassroomStudent ClassroomStudent::fromJson( const QJsonObject& object )
{
	ClassroomStudent student;
	student.seat = object.value( QStringLiteral("seat") ).toString();
	student.name = object.value( QStringLiteral("name") ).toString();
	student.ip = object.value( QStringLiteral("ip") ).toString();
	return student;
}


QJsonObject ClassroomClass::toJson() const
{
	QJsonArray studentsArray;
	for( const auto& student : students )
	{
		studentsArray.append( student.toJson() );
	}
	return { { QStringLiteral("name"), name }, { QStringLiteral("students"), studentsArray } };
}


ClassroomClass ClassroomClass::fromJson( const QJsonObject& object )
{
	ClassroomClass result;
	result.name = object.value( QStringLiteral("name") ).toString();
	const auto studentsArray = object.value( QStringLiteral("students") ).toArray();
	for( const auto& value : studentsArray )
	{
		result.students.append( ClassroomStudent::fromJson( value.toObject() ) );
	}
	return result;
}


ClassroomRoster::ClassroomRoster( const QString& dataDirectory ) :
	m_filePath( QDir( dataDirectory ).filePath( QStringLiteral("classes.json") ) )
{
	load();
}


QString ClassroomRoster::filePath() const
{
	return m_filePath;
}


bool ClassroomRoster::load()
{
	QFile file( m_filePath );
	if( file.exists() == false )
	{
		return false;
	}
	if( file.open( QFile::ReadOnly ) == false )
	{
		return false;
	}
	return fromJson( QString::fromUtf8( file.readAll() ) );
}


bool ClassroomRoster::save() const
{
	return writeFileAtomically( m_filePath,
		QJsonDocument( QJsonObject{ { QStringLiteral("classes"), toJsonArray() } } ).toJson( QJsonDocument::Indented ) );
}


QJsonArray ClassroomRoster::toJsonArray() const
{
	QJsonArray array;
	for( const auto& item : m_classes ) { array.append( item.toJson() ); }
	return array;
}


void ClassroomRoster::clear()
{
	m_classes.clear();
}


QStringList ClassroomRoster::classNames() const
{
	QStringList names;
	for( const auto& item : m_classes ) { names.append( item.name ); }
	return names;
}


ClassroomClass* ClassroomRoster::findClass( const QString& name )
{
	for( auto& item : m_classes )
	{
		if( item.name == name ) { return &item; }
	}
	return nullptr;
}


const ClassroomClass* ClassroomRoster::findClass( const QString& name ) const
{
	for( const auto& item : m_classes )
	{
		if( item.name == name ) { return &item; }
	}
	return nullptr;
}


ClassroomStudent* ClassroomRoster::findStudent( const QString& className, const QString& seat )
{
	auto* item = findClass( className );
	if( item == nullptr ) { return nullptr; }
	for( auto& student : item->students )
	{
		if( student.seat == seat ) { return &student; }
	}
	return nullptr;
}


ClassroomStudent* ClassroomRoster::findStudentByIp( const QString& className, const QString& ip )
{
	auto* item = findClass( className );
	if( item == nullptr ) { return nullptr; }
	for( auto& student : item->students )
	{
		if( student.ip.isEmpty() == false && student.ip == ip ) { return &student; }
	}
	return nullptr;
}


QString ClassroomRoster::toJson() const
{
	return QString::fromUtf8( QJsonDocument( QJsonObject{ { QStringLiteral("classes"), toJsonArray() } } ).toJson( QJsonDocument::Indented ) );
}


bool ClassroomRoster::fromJson( const QString& json )
{
	const auto document = QJsonDocument::fromJson( json.toUtf8() );
	if( document.isObject() == false ) { return false; }

	const auto root = document.object();
	const auto classesValue = root.value( QStringLiteral("classes") );
	if( classesValue.isArray() == false ) { return false; }

	QVector<ClassroomClass> staged;
	const auto stringField = []( const QJsonObject& object, const QString& name, QString& destination ) {
		const auto value = object.value( name );
		if( value.isUndefined() ) { return true; }
		if( value.isString() == false ) { return false; }
		destination = value.toString();
		return true;
	};

	for( const auto& classValue : classesValue.toArray() )
	{
		if( classValue.isObject() == false ) { return false; }
		const auto classObject = classValue.toObject();
		ClassroomClass item;
		if( stringField( classObject, QStringLiteral("name"), item.name ) == false ) { return false; }
		const auto studentsValue = classObject.value( QStringLiteral("students") );
		if( studentsValue.isArray() == false ) { return false; }
		for( const auto& studentValue : studentsValue.toArray() )
		{
			if( studentValue.isObject() == false ) { return false; }
			const auto studentObject = studentValue.toObject();
			ClassroomStudent student;
			if( stringField( studentObject, QStringLiteral("seat"), student.seat ) == false ||
				stringField( studentObject, QStringLiteral("name"), student.name ) == false ||
				stringField( studentObject, QStringLiteral("ip"), student.ip ) == false )
			{
				return false;
			}
			item.students.append( student );
		}
		staged.append( item );
	}

	m_classes = staged;
	return true;
}


// ---------------------------------------------------------------- table import

namespace
{

//! Excel on a Chinese Windows writes CSV in the ANSI code page, not UTF-8.
//! Trying strict UTF-8 first and falling back to the local 8 bit codec is what
//! makes "just save as CSV" work for a teacher.
QString decodeRosterText( const QByteArray& raw )
{
	auto bytes = raw;
	if( bytes.startsWith( "\xEF\xBB\xBF" ) ) { bytes.remove( 0, 3 ); }

#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
	QStringDecoder strict( QStringDecoder::Utf8 );
	auto text = strict.decode( bytes );
	if( strict.hasError() == false ) { return text; }
	QStringDecoder local( QStringDecoder::System );
	return local.decode( bytes );
#else
	const auto text = QString::fromUtf8( bytes );
	if( text.contains( QChar( 0xFFFD ) ) == false ) { return text; }
	return QString::fromLocal8Bit( bytes );
#endif
}


QChar detectSeparator( const QString& text )
{
	// Only look at the first few lines: an IP address is full of dots and a
	// lesson name may contain commas, so counting the whole file is wrong.
	const auto lines = text.split( QRegularExpression( QStringLiteral("[\\r\\n]") ), Qt::SkipEmptyParts );
	QMap<QChar, int> counts;
	for( int lineIndex = 0; lineIndex < qMin( 5, int( lines.size() ) ); ++lineIndex )
	{
		const auto& line = lines.at( lineIndex );
		for( const auto separator : { QLatin1Char(';'), QLatin1Char(','), QLatin1Char('\t') } )
		{
			counts[separator] += line.count( separator );
		}
	}

	QChar best = QLatin1Char(';');
	int bestCount = 0;
	for( auto it = counts.constBegin(); it != counts.constEnd(); ++it )
	{
		if( it.value() > bestCount )
		{
			best = it.key();
			bestCount = it.value();
		}
	}
	return bestCount > 0 ? best : QLatin1Char(';');
}


int findColumn( const QStringList& header, const QStringList& keywords )
{
	for( int index = 0; index < header.size(); ++index )
	{
		const auto value = header.at( index ).trimmed().toLower();
		if( value.isEmpty() ) { continue; }
		for( const auto& keyword : keywords )
		{
			if( value.contains( keyword ) ) { return index; }
		}
	}
	return -1;
}

//! Upper bounds for a teacher-selected import file. A roster of a whole school
//! stays far below them; they keep a stray gigabyte export or a million-row
//! paste from being decoded and split in memory in one go.
constexpr int MaxImportBytes = 8 * 1024 * 1024;
constexpr int MaxImportRows = 20000;
constexpr int MaxImportColumns = 64;
constexpr int MaxImportFieldLength = 256;


//! Parses the record starting at `position` into `fields` and advances
//! `position` and the 1-based `line` counter past it. A line break inside a
//! quoted cell belongs to that cell instead of ending the row. Returns false
//! once the text is exhausted.
bool nextTableRecord( const QString& text, int& position, QChar separator,
					  QStringList& fields, int& line, int& recordLine )
{
	recordLine = line;
	fields.clear();
	QString field;
	bool quoted = false;
	int index = position;
	const int size = text.size();
	while( index < size )
	{
		const auto character = text.at( index );
		if( quoted )
		{
			if( character == QLatin1Char('"') )
			{
				if( index + 1 < size && text.at( index + 1 ) == QLatin1Char('"') )
				{
					field += QLatin1Char('"');
					index += 2;
					continue;
				}
				quoted = false;
			}
			else
			{
				if( character == QLatin1Char('\n') ) { ++line; }
				if( character != QLatin1Char('\r') ) { field += character; }
			}
			++index;
			continue;
		}
		if( character == QLatin1Char('"') ) { quoted = true; ++index; continue; }
		if( character == separator ) { fields.append( field ); field.clear(); ++index; continue; }
		if( character == QLatin1Char('\r') || character == QLatin1Char('\n') ) { break; }
		field += character;
		++index;
	}
	fields.append( field );

	while( index < size && ( text.at( index ) == QLatin1Char('\r') || text.at( index ) == QLatin1Char('\n') ) )
	{
		if( text.at( index ) == QLatin1Char('\n') ) { ++line; }
		++index;
	}
	const bool progressed = index != position;
	position = index;
	return progressed;
}


bool isBlankRecord( const QStringList& fields )
{
	return fields.size() == 1 && fields.first().trimmed().isEmpty();
}


//! An IPv4/IPv6 address, or at least something shaped like a host name. Bare
//! "192.168.1" or "computer 05" would later be pushed to every machine as a
//! broken identity, so they are refused here.
bool isValidRosterAddress( const QString& address )
{
	if( QHostAddress( address ).isNull() == false ) { return true; }
	static const QRegularExpression numeric( QStringLiteral("^[0-9.]+$") );
	if( numeric.match( address ).hasMatch() ) { return false; }
	static const QRegularExpression hostName(
		QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?$") );
	return hostName.match( address ).hasMatch();
}

} // namespace


bool ClassroomRoster::importTable( const QString& text, const QString& defaultClassName,
								   ClassroomImportReport* report, QString* error )
{
	Q_UNUSED( error )
	const auto tr = []( const char* text ) {
		static const QHash<QByteArray, QString> chinese = {
			{ "No seat/name column found in the imported table.", QStringLiteral("导入表格中没有找到座位号或姓名列。") },
			{ "Line %1: no class name", QStringLiteral("第 %1 行：缺少班级名称") },
			{ "Line %1: no student name", QStringLiteral("第 %1 行：缺少学生姓名") },
			{ "Line %1: %2 already uses IP %3 in class %4", QStringLiteral("第 %1 行：%2 已在班级 %4 中使用 IP %3") },
			{ "Line %1: %2 is not a valid IP address", QStringLiteral("第 %1 行：%2 不是有效的 IP 地址") },
			{ "Line %1: row skipped (too many columns or an over-long cell)",
			  QStringLiteral("第 %1 行已跳过（列数过多或单元格过长）") },
			{ "The table has more than %1 rows; the remaining rows were not imported.",
			  QStringLiteral("表格超过 %1 行，其余行未导入。") }
		};
		return chinese.value( QByteArray( text ), QString::fromUtf8( text ) );
	};
	ClassroomImportReport localReport;
	auto& result = report ? *report : localReport;

	QStringList record;
	int position = 0;
	int line = 1;
	const auto separator = detectSeparator( text );

	int classColumn = -1;
	int seatColumn = -1;
	int nameColumn = -1;
	int ipColumn = -1;

	int recordLine = 1;
	const auto nextRecord = [&record, &position, &line, &recordLine, &text, separator]() {
		return nextTableRecord( text, position, separator, record, line, recordLine );
	};

	// Header detection: only the first non-empty record can be a header.
	bool have = nextRecord();
	while( have && isBlankRecord( record ) ) { have = nextRecord(); }
	if( have )
	{
		classColumn = findColumn( record, { QStringLiteral("班级"), QStringLiteral("班別"),
											QStringLiteral("class") } );
		seatColumn = findColumn( record, { QStringLiteral("座位"), QStringLiteral("座号"),
										   QStringLiteral("座號"), QStringLiteral("seat"),
										   QStringLiteral("序号") } );
		nameColumn = findColumn( record, { QStringLiteral("姓名"), QStringLiteral("学生"),
										   QStringLiteral("student"), QStringLiteral("name") } );
		ipColumn = findColumn( record, { QStringLiteral("ip") } );
		if( seatColumn >= 0 || nameColumn >= 0 )
		{
			have = nextRecord();
			while( have && isBlankRecord( record ) ) { have = nextRecord(); }
		}
		else
		{
			// No header: infer the layout from the column count of the first record.
			const auto width = record.size();
			if( width >= 4 )
			{
				classColumn = 0;
				seatColumn = 1;
				nameColumn = 2;
				ipColumn = 3;
			}
			else if( width == 3 )
			{
				seatColumn = 0;
				nameColumn = 1;
				ipColumn = 2;
			}
			else
			{
				seatColumn = 0;
				nameColumn = 1;
			}
		}
	}

	if( seatColumn < 0 || nameColumn < 0 )
	{
		result.note( tr( "No seat/name column found in the imported table." ) );
		return false;
	}

	// Rows go into a copy first: a file with nothing usable in it must leave
	// the roster exactly as it was, instead of keeping half-applied classes.
	QVector<ClassroomClass> staged = m_classes;
	const auto findStagedClass = [&staged]( const QString& name ) -> ClassroomClass* {
		for( auto& item : staged )
		{
			if( item.name == name ) { return &item; }
		}
		return nullptr;
	};

	int rows = 0;
	for( ; have; have = nextRecord() )
	{
		if( isBlankRecord( record ) ) { continue; }
		if( ++rows > MaxImportRows )
		{
			result.note( tr( "The table has more than %1 rows; the remaining rows were not imported." )
						 .arg( MaxImportRows ) );
			break;
		}
		if( record.size() > MaxImportColumns )
		{
			result.note( tr( "Line %1: row skipped (too many columns or an over-long cell)" ).arg( recordLine ) );
			continue;
		}
		bool overLongCell = false;
		for( const auto& field : record )
		{
			overLongCell = overLongCell || field.size() > MaxImportFieldLength;
		}
		if( overLongCell )
		{
			result.note( tr( "Line %1: row skipped (too many columns or an over-long cell)" ).arg( recordLine ) );
			continue;
		}
		const auto trimmed = [&record]( int column ) {
			return column >= 0 && column < record.size() ? record.at( column ).trimmed() : QString{};
		};

		auto className = trimmed( classColumn );
		if( className.isEmpty() ) { className = defaultClassName; }
		if( className.isEmpty() )
		{
			result.note( tr( "Line %1: no class name" ).arg( recordLine ) );
			continue;
		}

		ClassroomStudent student;
		student.seat = trimmed( seatColumn );
		student.name = trimmed( nameColumn );
		student.ip = trimmed( ipColumn );
		if( student.seat.isEmpty() && student.name.isEmpty() ) { continue; }
		if( student.name.isEmpty() )
		{
			result.note( tr( "Line %1: no student name" ).arg( recordLine ) );
			continue;
		}
		if( student.ip.isEmpty() == false && isValidRosterAddress( student.ip ) == false )
		{
			result.note( tr( "Line %1: %2 is not a valid IP address" ).arg( recordLine ).arg( student.ip ) );
			continue;
		}

		auto* item = findStagedClass( className );
		if( item == nullptr )
		{
			ClassroomClass newClass;
			newClass.name = className;
			staged.append( newClass );
			item = &staged.last();
			++result.classes;
		}

		// Same class, same seat: replace. Same class, same IP on ANOTHER seat:
		// that machine cannot host two students, so keep the first one and tell
		// the teacher instead of silently sending the wrong identity later.
		ClassroomStudent* sameSeat = nullptr;
		bool ipConflict = false;
		for( auto& existing : item->students )
		{
			if( student.seat.isEmpty() == false && existing.seat == student.seat ) { sameSeat = &existing; }
			else if( student.ip.isEmpty() == false && existing.ip == student.ip )
			{
				// Also raised when the row replaces its own seat: the IP may not
				// silently migrate from the seat that already owns it.
				ipConflict = true;
			}
		}
		if( ipConflict )
		{
			++result.ipConflicts;
			result.note( tr( "Line %1: %2 already uses IP %3 in class %4" )
							 .arg( recordLine ).arg( student.name, student.ip, className ) );
			continue;
		}
		if( sameSeat )
		{
			*sameSeat = student;
			++result.overwritten;
		}
		else
		{
			item->students.append( student );
			++result.students;
		}
	}

	if( result.students > 0 || result.overwritten > 0 )
	{
		m_classes = staged;
	}
	return result.students > 0 || result.overwritten > 0;
}


bool ClassroomRoster::importTableFile( const QString& fileName, const QString& defaultClassName,
									   ClassroomImportReport* report, QString* error )
{
	QFile file( fileName );
	if( file.open( QFile::ReadOnly ) == false )
	{
		if( error ) { *error = file.errorString(); }
		return false;
	}
	if( file.size() > MaxImportBytes )
	{
		if( error )
		{
			*error = QStringLiteral( "文件超过 %1 MB，无法作为名单表格导入。" ).arg( MaxImportBytes / ( 1024 * 1024 ) );
		}
		return false;
	}
	const auto bytes = file.read( MaxImportBytes + 1 );
	file.close();
	return importTable( decodeRosterText( bytes ), defaultClassName, report, error );
}


QByteArray ClassroomRoster::importTemplate()
{
	const auto text = QString::fromUtf8(
		"班级;座位号;姓名;IP地址\n"
		"3年级1班;1;张三;192.168.1.101\n"
		"3年级1班;2;李四;192.168.1.102\n"
		"3年级2班;1;王五;192.168.1.101\n" );
	QByteArray data( "\xEF\xBB\xBF", 3 );
	data += text.toUtf8();
	return data;
}


bool ClassroomRoster::importCsv( const QString& fileName, const QString& className, QString* error )
{
	// Legacy single-class entry point: keep the behaviour, but go through the
	// shared parser so quoted fields and GBK files work here too.
	ClassroomImportReport report;
	return importTableFile( fileName, className, &report, error );
}


bool ClassroomRoster::exportCsv( const QString& fileName, const QString& className, QString* error ) const
{
	const auto* item = findClass( className );
	if( item == nullptr ) { return true; }

	QVector<QStringList> rows;
	for( const auto& student : item->students )
	{
		rows.append( { student.seat, student.name, student.ip } );
	}
	return writeCsv( fileName, { QStringLiteral("seat"), QStringLiteral("name"), QStringLiteral("ip") }, rows, error );
}


QJsonObject ClassroomEvent::toJson() const
{
	return { { QStringLiteral("ts"), timestamp.toString( Qt::ISODateWithMs ) },
			 { QStringLiteral("session"), sessionId },
			 { QStringLiteral("class"), className },
			 { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("host"), host },
			 { QStringLiteral("ip"), ip },
			 { QStringLiteral("type"), type },
			 { QStringLiteral("app"), app },
			 { QStringLiteral("detail"), detail },
			 { QStringLiteral("category"), category } };
}


ClassroomEvent ClassroomEvent::fromJson( const QJsonObject& object )
{
	ClassroomEvent event;
	event.timestamp = QDateTime::fromString( object.value( QStringLiteral("ts") ).toString(), Qt::ISODateWithMs );
	event.sessionId = object.value( QStringLiteral("session") ).toString();
	event.className = object.value( QStringLiteral("class") ).toString();
	event.seat = object.value( QStringLiteral("seat") ).toString();
	event.student = object.value( QStringLiteral("student") ).toString();
	event.host = object.value( QStringLiteral("host") ).toString();
	event.ip = object.value( QStringLiteral("ip") ).toString();
	event.type = object.value( QStringLiteral("type") ).toString();
	event.app = object.value( QStringLiteral("app") ).toString();
	event.detail = object.value( QStringLiteral("detail") ).toString();
	event.category = object.value( QStringLiteral("category") ).toString();
	return event;
}


QString ClassroomLogStore::sanitize( const QString& name )
{
	auto result = name.trimmed();
	if( result.isEmpty() ) { return QStringLiteral("unknown"); }
	const QString invalid = QStringLiteral("\\/:*?\"<>|\r\n\t ");
	for( const auto& character : invalid )
	{
		result.replace( character, QLatin1Char('_') );
	}
	while( result.contains( QStringLiteral("__") ) ) { result.replace( QStringLiteral("__"), QStringLiteral("_") ); }
	return result.left( 96 );
}


//! Injective variant of sanitize() for storage keys. Score and shop books are
//! keyed on the class name: two different names must never map onto one file.
//! Event log paths keep the legacy sanitize() so existing installations do not
//! lose their log history.
QString ClassroomLogStore::storageKey( const QString& name )
{
	const auto legacy = sanitize( name );
	const auto trimmed = name.trimmed();
	if( legacy == trimmed && trimmed == name ) { return legacy; }
	const auto digest = QCryptographicHash::hash( name.toUtf8(), QCryptographicHash::Sha256 ).
						toHex().left( 8 );
	return legacy.left( 96 ) + QLatin1Char('-') + QString::fromLatin1( digest );
}


//! Moves a pre-5.3.1 book file from its lossy key to the injective one, so an
//! updated teacher machine keeps the existing score and shop data. Returns
//! false when nothing had to be migrated.
bool ClassroomLogStore::migrateStorageKey( const QString& directory, const QString& legacyName,
										   const QString& newName )
{
	if( newName == legacyName ) { return false; }
	QFile legacy( QDir( directory ).filePath( legacyName ) );
	QFile modern( QDir( directory ).filePath( newName ) );
	if( legacy.exists() == false || modern.exists() ) { return false; }
	if( legacy.rename( QFileInfo( modern ).absoluteFilePath() ) == false )
	{
		qWarning( "ikeback: could not migrate %s to %s", qPrintable( legacy.fileName() ),
				  qPrintable( modern.fileName() ) );
		return false;
	}
	return true;
}


ClassroomLogStore::ClassroomLogStore( const QString& baseDirectory, const QString& scope ) :
	m_basePath( QDir( baseDirectory ).filePath( sanitize( scope ) ) )
{
	QDir().mkpath( eventsPath() );
	QDir().mkpath( machinesPath() );
	QDir().mkpath( studentsPath() );
}


QString ClassroomLogStore::basePath() const { return m_basePath; }
QString ClassroomLogStore::eventsPath() const { return QDir( m_basePath ).filePath( QStringLiteral("events") ); }
QString ClassroomLogStore::machinesPath() const { return QDir( m_basePath ).filePath( QStringLiteral("machines") ); }
QString ClassroomLogStore::studentsPath() const { return QDir( m_basePath ).filePath( QStringLiteral("students") ); }


bool ClassroomLogStore::writeLine( const QString& fileName, const QByteArray& line )
{
	QFile file( fileName );
	if( file.open( QFile::WriteOnly | QFile::Append | QFile::Text ) == false )
	{
		return false;
	}
	return file.write( line ) == line.size();
}


bool ClassroomLogStore::append( const ClassroomEvent& event )
{
	QMutexLocker locker( &m_mutex );

	const auto timestamp = event.timestamp.isValid() ? event.timestamp : QDateTime::currentDateTime();
	const auto day = timestamp.date().toString( QStringLiteral("yyyy-MM-dd") );
	const auto line = QJsonDocument( event.toJson() ).toJson( QJsonDocument::Compact ) + QByteArrayLiteral("\n");

	const auto hostDirectory = QDir( machinesPath() ).filePath( sanitize( event.host ) );
	const auto studentName = event.student.isEmpty() ? QStringLiteral("unassigned") : event.student;
	const auto studentDirectory = QDir( QDir( studentsPath() ).filePath( sanitize( event.className ) ) ).
								   filePath( sanitize( event.seat + QLatin1Char('-') + studentName ) );

	const auto ensureDirectory = [this]( const QString& path ) {
		if( m_knownDirectories.contains( path ) ) { return; }
		QDir().mkpath( path );
		m_knownDirectories.insert( path );
	};
	ensureDirectory( hostDirectory );
	ensureDirectory( studentDirectory );

	bool ok = true;
	ok &= writeLine( QDir( eventsPath() ).filePath( day + QStringLiteral(".jsonl") ), line );
	ok &= writeLine( QDir( hostDirectory ).filePath( day + QStringLiteral(".jsonl") ), line );
	ok &= writeLine( QDir( studentDirectory ).filePath( day + QStringLiteral(".jsonl") ), line );
	ensureIndex();
	indexEvent( event );
	return ok;
}


void ClassroomLogStore::ensureIndex() const
{
	if( m_indexLoaded ) { return; }
	m_indexLoaded = true;

	auto day = QDate::currentDate().addDays( -1 );
	const auto end = QDate::currentDate();
	while( day <= end )
	{
		const auto fileName = QDir( eventsPath() ).
							  filePath( day.toString( QStringLiteral("yyyy-MM-dd") ) + QStringLiteral(".jsonl") );
		for( const auto& event : readDayFile( fileName ) ) { indexEvent( event ); }
		day = day.addDays( 1 );
	}
}


void ClassroomLogStore::indexEvent( const ClassroomEvent& event ) const
{
	if( event.ip.isEmpty() ) { return; }
	if( event.timestamp.isValid() &&
		( m_lastActivities.contains( event.ip ) == false || event.timestamp > m_lastActivities.value( event.ip ) ) )
	{
		m_lastActivities[event.ip] = event.timestamp;
	}

	if( event.type == Classroom::EventType::login() || event.type == Classroom::EventType::assign() ||
		event.type == Classroom::EventType::switchUser() )
	{
		m_identityStates[event.ip] = QStringLiteral("%1 (%2)").arg( event.student, event.seat );
	}
	else if( event.type == Classroom::EventType::logout() )
	{
		m_identityStates.remove( event.ip );
	}
}


QMap<QString, QString> ClassroomLogStore::identityStates() const
{
	QMutexLocker locker( &m_mutex );
	ensureIndex();
	return m_identityStates;
}


QMap<QString, QDateTime> ClassroomLogStore::lastActivities() const
{
	QMutexLocker locker( &m_mutex );
	ensureIndex();
	return m_lastActivities;
}


QVector<ClassroomEvent> ClassroomLogStore::readDayFile( const QString& fileName )
{
	QVector<ClassroomEvent> events;
	QFile file( fileName );
	if( file.open( QFile::ReadOnly | QFile::Text ) == false ) { return events; }

	while( file.atEnd() == false )
	{
		const auto line = file.readLine().trimmed();
		if( line.isEmpty() ) { continue; }
		const auto document = QJsonDocument::fromJson( line );
		if( document.isObject() ) { events.append( ClassroomEvent::fromJson( document.object() ) ); }
	}
	return events;
}


QVector<ClassroomEvent> ClassroomLogStore::load( const QDate& from, const QDate& to, const QString& machine ) const
{
	QMutexLocker locker( &m_mutex );

	const auto start = from.isValid() ? from : QDate::currentDate().addDays( -30 );
	const auto end = to.isValid() ? to : QDate::currentDate();
	const auto directory = machine.isEmpty() ? eventsPath() : QDir( machinesPath() ).filePath( sanitize( machine ) );

	QVector<ClassroomEvent> events;
	auto day = start;
	int guard = 0;
	while( day <= end && guard++ < 3660 )
	{
		const auto fileName = QDir( directory ).filePath( day.toString( QStringLiteral("yyyy-MM-dd") ) + QStringLiteral(".jsonl") );
		events += readDayFile( fileName );
		day = day.addDays( 1 );
	}
	return events;
}


QStringList ClassroomLogStore::machines() const
{
	QMutexLocker locker( &m_mutex );
	return QDir( machinesPath() ).entryList( QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name );
}


int ClassroomLogStore::purgeExpired( int days )
{
	if( days <= 0 ) { return 0; }
	QMutexLocker locker( &m_mutex );

	const auto cutoff = QDate::currentDate().addDays( -days );
	QStringList directories{ eventsPath() };
	for( const auto& machine : QDir( machinesPath() ).entryList( QDir::Dirs | QDir::NoDotAndDotDot ) )
	{
		directories += QDir( machinesPath() ).filePath( machine );
	}
	for( const auto& className : QDir( studentsPath() ).entryList( QDir::Dirs | QDir::NoDotAndDotDot ) )
	{
		const auto classPath = QDir( studentsPath() ).filePath( className );
		for( const auto& student : QDir( classPath ).entryList( QDir::Dirs | QDir::NoDotAndDotDot ) )
		{
			directories += QDir( classPath ).filePath( student );
		}
	}

	int removed = 0;
	for( const auto& directory : directories )
	{
		for( const auto& fileName : QDir( directory ).entryList( { QStringLiteral("*.jsonl") }, QDir::Files ) )
		{
			const auto date = QDate::fromString( QFileInfo( fileName ).completeBaseName(), QStringLiteral("yyyy-MM-dd") );
			if( date.isValid() && date < cutoff )
			{
				if( QFile::remove( QDir( directory ).filePath( fileName ) ) ) { ++removed; }
			}
		}
	}
	return removed;
}


bool ClassroomLogStore::exportCsv( const QString& fileName, const QVector<ClassroomEvent>& events, QString* error )
{
	QVector<QStringList> rows;
	for( const auto& event : events )
	{
		rows.append( { event.timestamp.toString( Qt::ISODateWithMs ), event.className, event.seat, event.student,
					   event.host, event.ip, event.sessionId, event.type, event.category, event.app, event.detail } );
	}
	return writeCsv( fileName,
					 { QStringLiteral("time"), QStringLiteral("class"), QStringLiteral("seat"), QStringLiteral("student"),
					   QStringLiteral("host"), QStringLiteral("ip"), QStringLiteral("session"), QStringLiteral("type"),
					   QStringLiteral("category"), QStringLiteral("app"), QStringLiteral("detail") },
					 rows, error );
}


bool ClassroomLogStore::exportProfilesCsv( const QString& fileName, const QStringList& header,
										   const QVector<QStringList>& rows, QString* error )
{
	return writeCsv( fileName, header, rows, error );
}


QString ClassroomInterestAnalyzer::defaultCategoriesJson()
{
	return QString::fromUtf8( R"json([
  {"name":"编程与科技","keywords":"python,c++,java,github,stackoverflow,csdn,leetcode,vscode,visual studio,codeblocks,dev-c++,编程,代码,算法,人工智能,ai,机器人,科创","layer":"科技探索型"},
  {"name":"学科学习","keywords":"数学,物理,化学,生物,英语,语文,历史,地理,作业,题库,网课,mooc,课件,教材,考试,学习,习题,可汗","layer":"学业深耕型"},
  {"name":"阅读写作","keywords":"word,文档,作文,阅读,小说,知乎,豆瓣,笔记,markdown,写作,博客,论文,读书","layer":"阅读写作型"},
  {"name":"艺术设计","keywords":"photoshop,figma,绘画,设计,插画,blender,剪映,premiere,视频剪辑,建模,海报,3d","layer":"创意设计型"},
  {"name":"游戏娱乐","keywords":"steam,游戏,原神,王者荣耀,minecraft,lol,epic,小游戏,网吧,攻略,我的世界","layer":"游戏兴趣型"},
  {"name":"短视频与社交","keywords":"bilibili,b站,抖音,快手,微博,qq,微信,直播,短视频,tiktok,youtube,小红书,贴吧","layer":"社交媒介型"},
  {"name":"体育健康","keywords":"体育,篮球,足球,健身,跑步,nba,运动,羽毛球,游泳,赛事","layer":"运动健康型"},
  {"name":"音乐影视","keywords":"音乐,电影,影视,网易云,spotify,钢琴,吉他,动漫,追剧,演唱会","layer":"艺术欣赏型"}
])json" );
}


QVector<ClassroomInterestCategory> ClassroomInterestAnalyzer::categoriesFromJson( const QString& json )
{
	const auto document = QJsonDocument::fromJson( ( json.isEmpty() ? defaultCategoriesJson() : json ).toUtf8() );
	QVector<ClassroomInterestCategory> categories;
	for( const auto& value : document.array() )
	{
		const auto object = value.toObject();
		ClassroomInterestCategory category;
		category.name = object.value( QStringLiteral("name") ).toString();
		category.layer = object.value( QStringLiteral("layer") ).toString();
		const auto keywords = object.value( QStringLiteral("keywords") );
		if( keywords.isArray() )
		{
			for( const auto& keyword : keywords.toArray() ) { category.keywords += keyword.toString().trimmed().toLower(); }
		}
		else
		{
			for( const auto& keyword : keywords.toString().split( QLatin1Char(','), Qt::SkipEmptyParts ) )
			{
				category.keywords += keyword.trimmed().toLower();
			}
		}
		if( category.name.isEmpty() == false ) { categories.append( category ); }
	}
	if( categories.isEmpty() )
	{
		return categoriesFromJson( defaultCategoriesJson() );
	}
	return categories;
}


QString ClassroomInterestAnalyzer::categoriesToJson( const QVector<ClassroomInterestCategory>& categories )
{
	QJsonArray array;
	for( const auto& category : categories )
	{
		array.append( QJsonObject{ { QStringLiteral("name"), category.name },
								   { QStringLiteral("keywords"), category.keywords.join( QLatin1Char(',') ) },
								   { QStringLiteral("layer"), category.layer } } );
	}
	return QString::fromUtf8( QJsonDocument( array ).toJson( QJsonDocument::Indented ) );
}


QString ClassroomInterestAnalyzer::categorize( const QString& text, const QVector<ClassroomInterestCategory>& categories )
{
	const auto haystack = text.toLower();
	for( const auto& category : categories )
	{
		for( const auto& keyword : category.keywords )
		{
			if( keyword.isEmpty() == false && haystack.contains( keyword ) ) { return category.name; }
		}
	}
	return QStringLiteral("其他");
}


QVector<ClassroomInterestProfile> ClassroomInterestAnalyzer::analyze( const QVector<ClassroomEvent>& events,
																	 const QVector<ClassroomInterestCategory>& categories,
																	 int halfLifeDays )
{
	const auto halfLife = std::max( 1, halfLifeDays );
	const auto now = QDateTime::currentDateTime();
	QMap<QString, ClassroomInterestProfile> profiles;

	QDateTime earliest = now;
	QDateTime latest = QDateTime::fromSecsSinceEpoch( 0 );
	for( const auto& event : events )
	{
		if( event.timestamp.isValid() == false ) { continue; }
		if( event.type != Classroom::EventType::app() && event.type != Classroom::EventType::window() &&
			event.type != Classroom::EventType::web() && event.type != Classroom::EventType::ai() ) { continue; }
		if( event.timestamp < earliest ) { earliest = event.timestamp; }
		if( event.timestamp > latest ) { latest = event.timestamp; }
	}
	if( latest < earliest ) { latest = earliest; }
	const auto midpoint = earliest.addSecs( earliest.secsTo( latest ) / 2 );

	for( const auto& event : events )
	{
		if( event.timestamp.isValid() == false || event.student.isEmpty() ) { continue; }
		if( event.type != Classroom::EventType::app() && event.type != Classroom::EventType::window() &&
			event.type != Classroom::EventType::web() && event.type != Classroom::EventType::ai() ) { continue; }

		const QString key = event.className + QLatin1Char('|') + event.seat + QLatin1Char('|') + event.student;
		auto& profile = profiles[key];
		if( profile.key.isEmpty() )
		{
			profile.key = key;
			profile.className = event.className;
			profile.seat = event.seat;
			profile.student = event.student;
		}
		++profile.events;

		const auto ageDays = static_cast<double>( std::max<qint64>( 0, event.timestamp.daysTo( now ) ) );
		const auto weight = std::pow( 0.5, ageDays / static_cast<double>( halfLife ) );
		const QString combined = event.detail + QLatin1Char(' ') + event.app;
		const auto haystack = combined.toLower();

		for( const auto& category : categories )
		{
			for( const auto& keyword : category.keywords )
			{
				if( keyword.isEmpty() == false && haystack.contains( keyword ) )
				{
					profile.scores[category.name] += weight;
					if( event.timestamp >= midpoint ) { profile.trend += weight; }
					else { profile.trend -= weight; }
					break;
				}
			}
		}
	}

	QVector<ClassroomInterestProfile> result;
	for( auto& profile : profiles )
	{
		QSet<QString> sessions;
		for( const auto& event : events )
		{
			if( event.className == profile.className && event.seat == profile.seat && event.student == profile.student )
			{
				sessions.insert( event.sessionId.isEmpty() ? event.timestamp.date().toString( Qt::ISODate ) : event.sessionId );
			}
		}
		profile.sessions = sessions.size();

		double total = 0.0;
		double highest = 0.0;
		QString layer;
		for( auto it = profile.scores.constBegin(); it != profile.scores.constEnd(); ++it )
		{
			total += it.value();
			if( it.value() > highest )
			{
				highest = it.value();
				profile.topCategory = it.key();
			}
		}
		for( const auto& category : categories )
		{
			if( category.name == profile.topCategory ) { layer = category.layer; break; }
		}

		const auto entertainment = profile.scores.value( QStringLiteral("游戏娱乐") ) +
								   profile.scores.value( QStringLiteral("短视频与社交") );
		if( total <= 0.0 )
		{
			profile.suggestion = QStringLiteral("样本不足，建议继续观察");
		}
		else if( entertainment / total > 0.5 )
		{
			profile.suggestion = QStringLiteral("娱乐偏好偏高，建议引导 + 兴趣迁移");
		}
		else if( highest / total < 0.35 )
		{
			profile.suggestion = QStringLiteral("综合均衡型，适合项目式/跨层次任务");
		}
		else
		{
			profile.suggestion = layer.isEmpty() ? QStringLiteral("按主导兴趣分层") : layer;
		}

		profile.trend = total > 0.0 ? ( profile.trend / total ) * 100.0 : 0.0;
		result.append( profile );
	}

	std::sort( result.begin(), result.end(), []( const ClassroomInterestProfile& a, const ClassroomInterestProfile& b ) {
		auto sum = []( const QMap<QString, double>& scores ) {
			double value = 0.0;
			for( auto it = scores.constBegin(); it != scores.constEnd(); ++it ) { value += it.value(); }
			return value;
		};
		return sum( a.scores ) > sum( b.scores );
	} );

	return result;
}
