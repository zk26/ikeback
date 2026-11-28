/*
 * ClassroomScore.cpp - lesson based scoring with rich text details
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <algorithm>

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include "ClassroomData.h"
#include "ClassroomScore.h"


static QString trText( const char* text )
{
	// 这些文案通过变量传给 translate，lupdate 抓不到，构建出来的中文包里
	// 仍是英文。这里直接给出中文映射，保证「我的成绩」页面全中文。
	static const QHash<QByteArray, QString> chinese = {
		{ "Current score", QStringLiteral("当前积分") },
		{ "Base score", QStringLiteral("基础分") },
		{ "Score details", QStringLiteral("积分明细") },
		{ "No entries yet.", QStringLiteral("暂无记录") },
		{ "Time", QStringLiteral("时间") },
		{ "Lesson", QStringLiteral("课堂") },
		{ "Detail", QStringLiteral("说明") },
		{ "Delta", QStringLiteral("变动") },
		{ "Scoring rules", QStringLiteral("评分规则") }
	};
	return chinese.value( QByteArray( text ), QString::fromUtf8( text ) );
}


static QJsonArray toLessonArray( const QVector<ClassroomScoreLesson>& lessons )
{
	QJsonArray array;
	for( const auto& lesson : lessons ) { array.append( lesson.toJson() ); }
	return array;
}


static QJsonArray toEntryArray( const QVector<ClassroomScoreEntry>& entries )
{
	QJsonArray array;
	for( const auto& entry : entries ) { array.append( entry.toJson() ); }
	return array;
}


static QJsonArray toAttendanceArray( const QVector<ClassroomAttendance>& attendance )
{
	QJsonArray array;
	for( const auto& item : attendance ) { array.append( item.toJson() ); }
	return array;
}


static QJsonArray toQuickActionArray( const QVector<QPair<QString, int>>& actions )
{
	QJsonArray array;
	for( const auto& action : actions )
	{
		array.append( QJsonArray{ action.first, action.second } );
	}
	return array;
}


QJsonObject ClassroomScoreLesson::toJson() const
{
	return { { QStringLiteral("id"), id },
			 { QStringLiteral("date"), date.toString( Qt::ISODate ) },
			 { QStringLiteral("topic"), topic } };
}


ClassroomScoreLesson ClassroomScoreLesson::fromJson( const QJsonObject& object )
{
	ClassroomScoreLesson lesson;
	lesson.id = object.value( QStringLiteral("id") ).toString();
	lesson.date = QDate::fromString( object.value( QStringLiteral("date") ).toString(), Qt::ISODate );
	lesson.topic = object.value( QStringLiteral("topic") ).toString();
	return lesson;
}


QJsonObject ClassroomScoreEntry::toJson() const
{
	return { { QStringLiteral("id"), id },
			 { QStringLiteral("timestamp"), timestamp.toString( Qt::ISODateWithMs ) },
			 { QStringLiteral("lessonId"), lessonId },
			 { QStringLiteral("className"), className },
			 { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("delta"), delta },
			 { QStringLiteral("label"), label },
			 { QStringLiteral("richText"), richText },
			 { QStringLiteral("author"), author } };
}


ClassroomScoreEntry ClassroomScoreEntry::fromJson( const QJsonObject& object )
{
	ClassroomScoreEntry entry;
	entry.id = object.value( QStringLiteral("id") ).toString();
	entry.timestamp = QDateTime::fromString( object.value( QStringLiteral("timestamp") ).toString(), Qt::ISODateWithMs );
	entry.lessonId = object.value( QStringLiteral("lessonId") ).toString();
	entry.className = object.value( QStringLiteral("className") ).toString();
	entry.seat = object.value( QStringLiteral("seat") ).toString();
	entry.student = object.value( QStringLiteral("student") ).toString();
	entry.delta = object.value( QStringLiteral("delta") ).toInt();
	entry.label = object.value( QStringLiteral("label") ).toString();
	entry.richText = object.value( QStringLiteral("richText") ).toString();
	entry.author = object.value( QStringLiteral("author") ).toString();
	return entry;
}


QJsonObject ClassroomAttendance::toJson() const
{
	return { { QStringLiteral("lessonId"), lessonId },
			 { QStringLiteral("className"), className },
			 { QStringLiteral("seat"), seat },
			 { QStringLiteral("student"), student },
			 { QStringLiteral("status"), status },
			 { QStringLiteral("reason"), reason },
			 { QStringLiteral("author"), author },
			 { QStringLiteral("checkInTime"), checkInTime.toString( Qt::ISODateWithMs ) },
			 { QStringLiteral("updated"), updated.toString( Qt::ISODateWithMs ) } };
}


ClassroomAttendance ClassroomAttendance::fromJson( const QJsonObject& object )
{
	ClassroomAttendance attendance;
	attendance.lessonId = object.value( QStringLiteral("lessonId") ).toString();
	attendance.className = object.value( QStringLiteral("className") ).toString();
	attendance.seat = object.value( QStringLiteral("seat") ).toString();
	attendance.student = object.value( QStringLiteral("student") ).toString();
	attendance.status = qBound( int( Present ), object.value( QStringLiteral("status") ).toInt( int( Absent ) ),
								int( Custom ) );
	attendance.reason = object.value( QStringLiteral("reason") ).toString();
	attendance.author = object.value( QStringLiteral("author") ).toString();
	attendance.checkInTime = QDateTime::fromString( object.value( QStringLiteral("checkInTime") ).toString(),
													 Qt::ISODateWithMs );
	attendance.updated = QDateTime::fromString( object.value( QStringLiteral("updated") ).toString(),
												 Qt::ISODateWithMs );
	return attendance;
}


QJsonObject ClassroomScoreRules::toJson() const
{
	QJsonArray levelArray;
	for( const auto& level : this->levels ) { levelArray.append( QJsonArray{ level.first, level.second } ); }
	return { { QStringLiteral("published"), published },
			 { QStringLiteral("baseScore"), baseScore },
			 { QStringLiteral("html"), html },
			 { QStringLiteral("updated"), updated.toString( Qt::ISODateWithMs ) },
			 { QStringLiteral("quickActions"), toQuickActionArray( quickActions ) },
			 { QStringLiteral("levelSchemeName"), levelSchemeName },
			 { QStringLiteral("levels"), levelArray } };
}


ClassroomScoreRules ClassroomScoreRules::fromJson( const QJsonObject& object )
{
	ClassroomScoreRules rules;
	rules.published = object.value( QStringLiteral("published") ).toBool( false );
	rules.baseScore = object.value( QStringLiteral("baseScore") ).toInt( 100 );
	rules.html = object.value( QStringLiteral("html") ).toString();
	rules.updated = QDateTime::fromString( object.value( QStringLiteral("updated") ).toString(), Qt::ISODateWithMs );
	const auto actions = object.value( QStringLiteral("quickActions") ).toArray();
	for( const auto& action : actions )
	{
		const auto pair = action.toArray();
		if( pair.size() >= 2 )
		{
			rules.quickActions.append( { pair.at( 0 ).toString(), pair.at( 1 ).toInt() } );
		}
	}
	rules.levelSchemeName = object.value( QStringLiteral("levelSchemeName") ).toString( rules.levelSchemeName );
	for( const auto& value : object.value( QStringLiteral("levels") ).toArray() )
	{
		const auto pair = value.toArray();
		if( pair.size() >= 2 && pair.at( 0 ).isDouble() )
		{
			rules.levels.append( { pair.at( 0 ).toInt(), pair.at( 1 ).toString() } );
		}
	}
	std::stable_sort( rules.levels.begin(), rules.levels.end(), []( const auto& a, const auto& b ) {
		return a.first < b.first;
	} );
	return rules;
}


ClassroomScoreBook::ClassroomScoreBook( const QString& dataDirectory ) :
	m_basePath( dataDirectory )
{
}


QString ClassroomScoreBook::filePath( const QString& className ) const
{
	const QString directory = QDir( m_basePath ).filePath( QStringLiteral("scores") );
	return QDir( directory ).filePath( ClassroomLogStore::storageKey( className ) + QStringLiteral(".json") );
}


//! Old installations stored the book under the lossy sanitize() name.
void ClassroomScoreBook::migrateLegacyFile( const QString& className ) const
{
	const QString directory = QDir( m_basePath ).filePath( QStringLiteral("scores") );
	ClassroomLogStore::migrateStorageKey( directory,
					   ClassroomLogStore::sanitize( className ) + QStringLiteral(".json"),
					   ClassroomLogStore::storageKey( className ) + QStringLiteral(".json") );
}


bool ClassroomScoreBook::load( const QString& className )
{
	const auto modernFileName = filePath( className );
	const auto legacyFileName = QDir( QDir( m_basePath ).filePath( QStringLiteral("scores") ) ).
		filePath( ClassroomLogStore::sanitize( className ) + QStringLiteral(".json") );
	// Read a legacy file in place first. Renaming it before validation would
	// mutate disk even when the document turns out to be corrupt.
	const auto fileName = QFile::exists( modernFileName ) == false && QFile::exists( legacyFileName ) ?
							  legacyFileName : modernFileName;
	QFile file( fileName );
	if( file.exists() == false )
	{
		if( className.isEmpty() == false && className == m_className && m_persisted ) { return false; }
		m_className = className;
		m_persisted = false;
		m_lessons.clear();
		m_entries.clear();
		m_attendance.clear();
		m_rules = ClassroomScoreRules{};
		return true;
	}
	if( file.open( QFile::ReadOnly ) == false ) { return false; }

	const auto bytes = file.readAll();
	const auto readError = file.error();
	file.close();
	if( readError != QFile::NoError ) { return false; }

	const auto document = QJsonDocument::fromJson( bytes );
	if( document.isObject() == false ) { return false; }
	const auto object = document.object();

	const auto stringFieldsValid = []( const QJsonObject& value, const QStringList& names ) {
		for( const auto& name : names )
		{
			const auto field = value.value( name );
			if( field.isUndefined() == false && field.isString() == false ) { return false; }
		}
		return true;
	};
	const auto numberFieldsValid = []( const QJsonObject& value, const QStringList& names ) {
		for( const auto& name : names )
		{
			const auto field = value.value( name );
			if( field.isUndefined() == false && field.isDouble() == false ) { return false; }
		}
		return true;
	};

	const auto requiredStringFieldsValid = []( const QJsonObject& value, const QStringList& names ) {
		for( const auto& name : names )
		{
			const auto field = value.value( name );
			if( field.isString() == false ) { return false; }
		}
		return true;
	};

	ClassroomScoreRules stagedRules;
	if( object.contains( QStringLiteral("rules") ) )
	{
		const auto rules = object.value( QStringLiteral("rules") );
		if( rules.isObject() == false ) { return false; }
		const auto rulesObject = rules.toObject();
		if( stringFieldsValid( rulesObject, { QStringLiteral("html"), QStringLiteral("updated"),
											  QStringLiteral("levelSchemeName") } ) == false ||
			numberFieldsValid( rulesObject, { QStringLiteral("baseScore") } ) == false )
		{
			return false;
		}
		const auto published = rulesObject.value( QStringLiteral("published") );
		if( published.isUndefined() == false && published.isBool() == false ) { return false; }
		const auto quickActions = rulesObject.value( QStringLiteral("quickActions") );
		if( quickActions.isUndefined() == false && quickActions.isArray() == false ) { return false; }
		for( const auto& action : quickActions.toArray() )
		{
			const auto pair = action.toArray();
			if( action.isArray() == false || pair.size() < 2 || pair.at( 0 ).isString() == false ||
				pair.at( 1 ).isDouble() == false )
			{
				return false;
			}
		}
		const auto levels = rulesObject.value( QStringLiteral("levels") );
		if( levels.isUndefined() == false && levels.isArray() == false ) { return false; }
		for( const auto& level : levels.toArray() )
		{
			const auto pair = level.toArray();
			if( level.isArray() == false || pair.size() < 2 || pair.at( 0 ).isDouble() == false ||
				pair.at( 1 ).isString() == false )
			{
				return false;
			}
		}
		stagedRules = ClassroomScoreRules::fromJson( rulesObject );
	}

	QVector<ClassroomScoreLesson> stagedLessons;
	const auto lessons = object.value( QStringLiteral("lessons") );
	if( lessons.isArray() == false ) { return false; }
	for( const auto& value : lessons.toArray() )
	{
		if( value.isObject() == false ) { return false; }
		const auto lessonObject = value.toObject();
		if( requiredStringFieldsValid( lessonObject, { QStringLiteral("id"), QStringLiteral("date"),
											   QStringLiteral("topic") } ) == false )
		{
			return false;
		}
		stagedLessons.append( ClassroomScoreLesson::fromJson( lessonObject ) );
		if( stagedLessons.last().id.isEmpty() || stagedLessons.last().date.isValid() == false ) { return false; }
	}

	QVector<ClassroomScoreEntry> stagedEntries;
	const auto entries = object.value( QStringLiteral("entries") );
	if( entries.isArray() == false ) { return false; }
	for( const auto& value : entries.toArray() )
	{
		if( value.isObject() == false ) { return false; }
		const auto entryObject = value.toObject();
		if( requiredStringFieldsValid( entryObject, { QStringLiteral("id"), QStringLiteral("timestamp"),
											  QStringLiteral("lessonId"), QStringLiteral("className"),
											  QStringLiteral("seat"), QStringLiteral("student"),
											  QStringLiteral("label"), QStringLiteral("richText"),
											  QStringLiteral("author") } ) == false ||
			numberFieldsValid( entryObject, { QStringLiteral("delta") } ) == false )
		{
			return false;
		}
		stagedEntries.append( ClassroomScoreEntry::fromJson( entryObject ) );
		if( stagedEntries.last().id.isEmpty() || stagedEntries.last().lessonId.isEmpty() ||
			stagedEntries.last().seat.isEmpty() || stagedEntries.last().timestamp.isValid() == false ) { return false; }
	}

	QVector<ClassroomAttendance> stagedAttendance;
	const auto attendance = object.value( QStringLiteral("attendance") );
	if( attendance.isArray() == false ) { return false; }
	for( const auto& value : attendance.toArray() )
	{
		if( value.isObject() == false ) { return false; }
		const auto attendanceObject = value.toObject();
		if( requiredStringFieldsValid( attendanceObject, { QStringLiteral("lessonId"), QStringLiteral("className"),
													QStringLiteral("seat"), QStringLiteral("student"),
													QStringLiteral("reason"), QStringLiteral("author"),
													QStringLiteral("checkInTime"), QStringLiteral("updated") } ) == false ||
			numberFieldsValid( attendanceObject, { QStringLiteral("status") } ) == false )
		{
			return false;
		}
		stagedAttendance.append( ClassroomAttendance::fromJson( attendanceObject ) );
		if( stagedAttendance.last().lessonId.isEmpty() || stagedAttendance.last().seat.isEmpty() ) { return false; }
	}

	// Commit only after the whole document has parsed and passed shape checks.
	// A corrupt file must never erase the teacher's last known good state.
	if( fileName != modernFileName )
	{
		migrateLegacyFile( className );
	}
	m_className = className;
	m_rules = stagedRules;
	m_lessons = stagedLessons;
	m_entries = stagedEntries;
	m_attendance = stagedAttendance;
	m_persisted = true;
	return true;
}


bool ClassroomScoreBook::save() const
{
	const auto fileName = filePath( m_className );
	if( QDir().mkpath( QFileInfo( fileName ).absolutePath() ) == false ) { return false; }
	// Whole document or nothing: a full disk must not truncate the score book.
	QSaveFile file( fileName );
	if( file.open( QIODevice::WriteOnly ) == false ) { return false; }
	const QByteArray data = QJsonDocument( syncSnapshot() ).toJson( QJsonDocument::Indented );
	if( file.write( data ) != data.size() ) { file.cancelWriting(); return false; }
	if( file.commit() == false ) { return false; }
	m_persisted = true;
	return true;
}


bool ClassroomScoreBook::setRules( const ClassroomScoreRules& rules )
{
	m_rules = rules;
	m_rules.updated = QDateTime::currentDateTime();
	return save();
}


QString ClassroomScoreBook::ensureLesson( const QDate& date, const QString& topic )
{
	for( const auto& lesson : m_lessons )
	{
		if( lesson.date == date && lesson.topic == topic ) { return lesson.id; }
	}

	ClassroomScoreLesson lesson;
	lesson.id = QUuid::createUuid().toString( QUuid::WithoutBraces ).left( 12 );
	lesson.date = date;
	lesson.topic = topic;
	m_lessons.append( lesson );
	if( save() == false )
	{
		m_lessons.removeLast();
		return {};
	}
	return lesson.id;
}


bool ClassroomScoreBook::addEntry( const ClassroomScoreEntry& entry )
{
	ClassroomScoreEntry copy = entry;
	if( copy.id.isEmpty() ) { copy.id = QUuid::createUuid().toString( QUuid::WithoutBraces ); }
	if( copy.timestamp.isValid() == false ) { copy.timestamp = QDateTime::currentDateTime(); }
	if( copy.className.isEmpty() ) { copy.className = m_className; }
	if( copy.richText.isEmpty() ) { copy.richText = xmlEscape( copy.label ); }
	m_entries.append( copy );
	return save();
}


bool ClassroomScoreBook::removeEntry( const QString& entryId )
{
	for( int index = 0; index < m_entries.size(); ++index )
	{
		if( m_entries.at( index ).id == entryId )
		{
			m_entries.removeAt( index );
			return save();
		}
	}
	return false;
}


bool ClassroomScoreBook::setAttendance( const ClassroomAttendance& attendance )
{
	if( m_className.isEmpty() || attendance.lessonId.isEmpty() || attendance.seat.isEmpty() ||
		attendance.status < ClassroomAttendance::Present || attendance.status > ClassroomAttendance::Custom )
	{
		return false;
	}
	auto copy = attendance;
	if( copy.className.isEmpty() ) { copy.className = m_className; }
	if( copy.updated.isValid() == false ) { copy.updated = QDateTime::currentDateTime(); }
	for( int index = 0; index < m_attendance.size(); ++index )
	{
		if( m_attendance.at( index ).lessonId == copy.lessonId && m_attendance.at( index ).seat == copy.seat )
		{
			const auto previous = m_attendance.at( index );
			m_attendance[index] = copy;
			if( save() ) { return true; }
			m_attendance[index] = previous;
			return false;
		}
	}
	m_attendance.append( copy );
	if( save() ) { return true; }
	m_attendance.removeLast();
	return false;
}


ClassroomAttendance ClassroomScoreBook::attendanceFor( const QString& lessonId, const QString& seat ) const
{
	for( const auto& attendance : m_attendance )
	{
		if( attendance.lessonId == lessonId && attendance.seat == seat ) { return attendance; }
	}
	return {};
}


QString ClassroomScoreBook::attendanceText( const QString& lessonId, const QString& seat ) const
{
	const auto attendance = attendanceFor( lessonId, seat );
	switch( attendance.status )
	{
	case ClassroomAttendance::Present: return QStringLiteral("出勤");
	case ClassroomAttendance::Late: return QStringLiteral("迟到");
	case ClassroomAttendance::Leave: return QStringLiteral("请假");
	case ClassroomAttendance::Custom: return attendance.reason.isEmpty() ? QStringLiteral("自定义") : attendance.reason;
	default: return QStringLiteral("缺勤");
	}
}


QVector<QPair<int, QString>> ClassroomScoreBook::defaultLevels()
{
	return { { 0, QStringLiteral("重度危险玩家") },
			 { 60, QStringLiteral("无电脑玩家") },
			 { 70, QStringLiteral("危险玩家") },
			 { 80, QStringLiteral("轻度危险玩家") },
			 { 90, QStringLiteral("调皮玩家") },
			 { 100, QStringLiteral("积极玩家") },
			 { 200, QStringLiteral("达人玩家") },
			 { 300, QStringLiteral("大神玩家") },
			 { 500, QStringLiteral("超神玩家") } };
}


QString ClassroomScoreBook::levelName( int score ) const
{
	const auto levels = m_rules.levels.isEmpty() ? defaultLevels() : m_rules.levels;
	QString name = levels.isEmpty() ? QString{} : levels.first().second;
	for( const auto& level : levels )
	{
		if( score >= level.first ) { name = level.second; }
		else { break; }
	}
	return name;
}


int ClassroomScoreBook::total( const QString& seat, const QString& upToLessonId ) const
{
	auto ordered = m_lessons;
	std::stable_sort( ordered.begin(), ordered.end(),
					  []( const ClassroomScoreLesson& a, const ClassroomScoreLesson& b ) {
						  return a.date < b.date;
					  } );

	int lastLesson = ordered.size() - 1;
	if( upToLessonId.isEmpty() == false )
	{
		lastLesson = -1;
		for( int index = 0; index < ordered.size(); ++index )
		{
			if( ordered.at( index ).id == upToLessonId ) { lastLesson = index; break; }
		}
	}

	int score = m_rules.baseScore;
	for( const auto& entry : m_entries )
	{
		if( entry.seat != seat || entry.lessonId.isEmpty() ) { continue; }
		for( int index = 0; index <= lastLesson && index < ordered.size(); ++index )
		{
			if( ordered.at( index ).id == entry.lessonId )
			{
				score += entry.delta;
				break;
			}
		}
	}
	return score;
}


QVector<ClassroomScoreEntry> ClassroomScoreBook::entriesForCell( const QString& lessonId, const QString& seat ) const
{
	QVector<ClassroomScoreEntry> result;
	for( const auto& entry : m_entries )
	{
		if( entry.lessonId == lessonId && entry.seat == seat ) { result.append( entry ); }
	}
	return result;
}


QVector<ClassroomScoreEntry> ClassroomScoreBook::entriesForStudent( const QString& seat ) const
{
	QVector<ClassroomScoreEntry> result;
	for( const auto& entry : m_entries )
	{
		if( entry.seat == seat ) { result.append( entry ); }
	}
	return result;
}


QString ClassroomScoreBook::studentHtml( const QString& seat, const QString& student ) const
{
	const auto score = total( seat );
	const auto title = student.isEmpty() ? seat : student;

	QString html;
	html += QStringLiteral("<html><body style=\"font-family:'Microsoft YaHei',sans-serif;\">");
	html += QStringLiteral("<h2>%1</h2>").arg( xmlEscape( title ) );
	html += QStringLiteral("<p><b>%1 %2</b> &nbsp; <span style=\"color:#888;\">%3 %4</span></p>")
				.arg( trText("Current score" ), QString::number( score ),
					  trText("Base score" ), QString::number( m_rules.baseScore ) );
	html += QStringLiteral("<p><b>%1：</b>%2</p>").arg( QStringLiteral("等级"), xmlEscape( levelName( score ) ) );

	html += QStringLiteral("<hr><h3>%1</h3>").arg( trText("Score details" ) );
	const auto details = entriesForStudent( seat );
	if( details.isEmpty() )
	{
		html += QStringLiteral("<p style=\"color:#888;\">%1</p>").arg( trText("No entries yet." ) );
	}
	else
	{
		html += QStringLiteral("<table width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"4\">");
		html += QStringLiteral("<tr><th>%1</th><th>%2</th><th>%3</th><th>%4</th></tr>")
					.arg( trText("Time" ), trText("Lesson" ), trText("Detail" ), trText("Delta" ) );
		for( const auto& entry : details )
		{
			QString topic;
			for( const auto& lesson : m_lessons )
			{
				if( lesson.id == entry.lessonId )
				{
					topic = lesson.date.toString( QStringLiteral("MM.dd") ) + QLatin1Char(' ') + lesson.topic;
					break;
				}
			}
			html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
						.arg( entry.timestamp.toString( QStringLiteral("yyyy-MM-dd HH:mm") ),
							  xmlEscape( topic ), inlineImages( entry.richText, m_basePath ),
							  entry.delta > 0 ? QStringLiteral("+%1").arg( entry.delta ) : QString::number( entry.delta ) );
		}
		html += QStringLiteral("</table>");
	}

	if( m_rules.published )
	{
		html += QStringLiteral("<hr><h3>%1</h3>").arg( trText("Scoring rules" ) );
		html += inlineImages( m_rules.html, m_basePath );
	}
	return QStringLiteral("%1</body></html>").arg( html );
}


QJsonObject ClassroomScoreBook::syncSnapshot() const
{
	return { { QStringLiteral("className"), m_className },
			 { QStringLiteral("rules"), m_rules.toJson() },
			 { QStringLiteral("lessons"), toLessonArray( m_lessons ) },
			 { QStringLiteral("entries"), toEntryArray( m_entries ) },
			 { QStringLiteral("attendance"), toAttendanceArray( m_attendance ) } };
}


bool ClassroomScoreBook::applySnapshot( const QJsonObject& snapshot )
{
	const auto className = snapshot.value( QStringLiteral("className") ).toString();
	if( className.isEmpty() ) { return false; }
	m_className = className;
	m_rules = ClassroomScoreRules::fromJson( snapshot.value( QStringLiteral("rules") ).toObject() );
	m_lessons.clear();
	const auto lessons = snapshot.value( QStringLiteral("lessons") ).toArray();
	for( const auto& value : lessons ) { m_lessons.append( ClassroomScoreLesson::fromJson( value.toObject() ) ); }
	m_entries.clear();
	const auto entries = snapshot.value( QStringLiteral("entries") ).toArray();
	for( const auto& value : entries ) { m_entries.append( ClassroomScoreEntry::fromJson( value.toObject() ) ); }
	m_attendance.clear();
	const auto attendance = snapshot.value( QStringLiteral("attendance") ).toArray();
	for( const auto& value : attendance ) { m_attendance.append( ClassroomAttendance::fromJson( value.toObject() ) ); }
	return save();
}


bool ClassroomScoreBook::appendSnapshot( const QJsonObject& snapshot )
{
	const auto className = snapshot.value( QStringLiteral("className") ).toString();
	if( className.isEmpty() ) { return false; }
	if( m_className.isEmpty() ) { m_className = className; }
	else if( className != m_className ) { return false; }

	QSet<QString> knownIds;
	knownIds.reserve( m_entries.size() );
	for( const auto& entry : m_entries ) { knownIds.insert( entry.id ); }

	bool changed = false;
	int added = 0;
	for( const auto& value : snapshot.value( QStringLiteral("entries") ).toArray() )
	{
		if( added >= 500 ) { break; }
		auto entry = ClassroomScoreEntry::fromJson( value.toObject() );
		if( entry.id.isEmpty() || knownIds.contains( entry.id ) ) { continue; }
		if( entry.delta < -5 || entry.delta > 5 || entry.seat.isEmpty() || entry.seat.size() > 16 ) { continue; }
		if( entry.label.size() > 200 || entry.richText.size() > 4000 ) { continue; }
		if( entry.className.isEmpty() ) { entry.className = m_className; }
		m_entries.append( entry );
		knownIds.insert( entry.id );
		++added;
		changed = true;
	}
	return changed ? save() : true;
}


QString ClassroomScoreBook::inlineImages( const QString& html, const QString& dataDirectory )
{
	const QRegularExpression pattern( QStringLiteral("<img[^>]+src=[\"']([^\"']+)[\"']"),
									  QRegularExpression::CaseInsensitiveOption );
	const QDir root( dataDirectory );
	const auto rootPrefix = QDir::cleanPath( root.absolutePath() ) + QLatin1Char('/');
	QString result;
	int cursor = 0;
	auto it = pattern.globalMatch( html );
	while( it.hasNext() )
	{
		const auto match = it.next();
		const auto source = match.captured( 1 );
		const int sourceStart = match.capturedStart( 1 );
		const int sourceEnd = match.capturedEnd( 1 );

		QString uri;
		if( source.startsWith( QStringLiteral("data:"), Qt::CaseInsensitive ) == false )
		{
			// Rich text can arrive from the phone. Never inline an absolute
			// Windows path, a file:// URL, a drive letter/UNC path or anything
			// containing ".."; only small relative files inside the ikeback
			// data directory are allowed.
			const auto relative = source.trimmed();
			const bool safe = relative.isEmpty() == false &&
							  QFileInfo( relative ).isRelative() &&
							  relative.contains( QStringLiteral("..") ) == false &&
							  relative.contains( QLatin1Char(':') ) == false &&
							  relative.startsWith( QLatin1Char('/') ) == false &&
							  relative.startsWith( QLatin1Char('\\') ) == false;
			if( safe )
			{
				const auto fileName = QDir::cleanPath( root.absoluteFilePath( relative ) );
				const QFileInfo imageInfo( fileName );
				if( fileName.startsWith( rootPrefix, Qt::CaseInsensitive ) &&
					imageInfo.isFile() && imageInfo.size() <= 2 * 1024 * 1024 )
				{
					QFile image( fileName );
					if( image.open( QFile::ReadOnly ) )
					{
						const auto bytes = image.readAll();
						const auto mime = fileName.endsWith( QStringLiteral(".png"), Qt::CaseInsensitive ) ?
											  QStringLiteral("image/png") : QStringLiteral("image/jpeg");
						uri = QStringLiteral("data:%1;base64,%2").arg( mime, QString::fromLatin1( bytes.toBase64() ) );
					}
				}
			}
		}

		result += html.mid( cursor, sourceStart - cursor );
		result += uri.isEmpty() ? source : uri;
		cursor = sourceEnd;
	}
	result += html.mid( cursor );
	return result;
}


QString ClassroomScoreBook::xmlEscape( const QString& text )
{
	QString result = text;
	result.replace( QLatin1Char('&'), QStringLiteral("&amp;") );
	result.replace( QLatin1Char('<'), QStringLiteral("&lt;") );
	result.replace( QLatin1Char('>'), QStringLiteral("&gt;") );
	result.replace( QLatin1Char('"'), QStringLiteral("&quot;") );
	return result;
}
