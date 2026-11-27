#include "ClassroomTheme.h"

#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QWidget>

namespace Classroom
{

QColor Theme::color( ColorRole role )
{
	const auto dark = QGuiApplication::palette().color( QPalette::Window ).lightness() < 128;
	switch( role )
	{
	case ColorRole::Background: return dark ? QColor( "#2b2b2b" ) : QColor( "#fff9e8" );
	case ColorRole::Surface: return dark ? QColor( "#333333" ) : QColor( "#ffffff" );
	case ColorRole::SurfaceAlternate: return dark ? QColor( "#3d3d3d" ) : QColor( "#fff3cd" );
	case ColorRole::Text: return dark ? QColor( "#f5f5f5" ) : QColor( "#4d3b2f" );
	case ColorRole::Muted: return dark ? QColor( "#cfcfcf" ) : QColor( "#8a7568" );
	case ColorRole::Border: return dark ? QColor( "#5a5a5a" ) : QColor( "#f0d9a6" );
	case ColorRole::Primary: return QColor( "#ffb300" );
	case ColorRole::PrimaryHover: return QColor( "#ffca28" );
	case ColorRole::Secondary: return QColor( "#81d4fa" );
	case ColorRole::Accent: return QColor( "#f48fb1" );
	case ColorRole::Success: return QColor( "#66bb6a" );
	case ColorRole::Warning: return QColor( "#ffa726" );
	case ColorRole::Danger: return QColor( "#ef5350" );
	case ColorRole::Disabled: return dark ? QColor( "#555555" ) : QColor( "#e7e0d4" );
	}
	return {};
}


void Theme::apply( QWidget* widget )
{
	if( widget == nullptr ) { return; }
	QFile file( QStringLiteral(":/classroom/theme.qss") );
	if( file.open( QIODevice::ReadOnly ) == false ) { return; }
	auto sheet = QString::fromUtf8( file.readAll() );
	const auto replace = [&sheet]( const QString& token, const QColor& color ) {
		sheet.replace( token, color.name() );
	};
	replace( QStringLiteral("%BACKGROUND%"), color( ColorRole::Background ) );
	replace( QStringLiteral("%SURFACE%"), color( ColorRole::Surface ) );
	replace( QStringLiteral("%SURFACE_ALT%"), color( ColorRole::SurfaceAlternate ) );
	replace( QStringLiteral("%TEXT%"), color( ColorRole::Text ) );
	replace( QStringLiteral("%MUTED%"), color( ColorRole::Muted ) );
	replace( QStringLiteral("%BORDER%"), color( ColorRole::Border ) );
	replace( QStringLiteral("%PRIMARY%"), color( ColorRole::Primary ) );
	replace( QStringLiteral("%PRIMARY_HOVER%"), color( ColorRole::PrimaryHover ) );
	replace( QStringLiteral("%SECONDARY%"), color( ColorRole::Secondary ) );
	replace( QStringLiteral("%ACCENT%"), color( ColorRole::Accent ) );
	replace( QStringLiteral("%SUCCESS%"), color( ColorRole::Success ) );
	replace( QStringLiteral("%WARNING%"), color( ColorRole::Warning ) );
	replace( QStringLiteral("%DANGER%"), color( ColorRole::Danger ) );
	replace( QStringLiteral("%DISABLED%"), color( ColorRole::Disabled ) );
	widget->setStyleSheet( sheet );
}

}
