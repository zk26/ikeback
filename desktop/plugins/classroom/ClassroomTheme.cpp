#include "ClassroomTheme.h"

#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Classroom
{
namespace
{

double linearChannel( double value )
{
	const auto channel = value / 255.0;
	return channel <= 0.04045 ? channel / 12.92 :
		std::pow( ( channel + 0.055 ) / 1.055, 2.4 );
}


QColor withLightness( const QColor& color, int lightness )
{
	auto adjusted = color;
	adjusted.setHsl( adjusted.hue(), adjusted.saturation(), qBound( 0, lightness, 255 ) );
	return adjusted;
}


}


QColor Theme::accessibleColor( const QColor& color, const QVector<QColor>& backgrounds, double minimum )
{
	if( backgrounds.isEmpty() ) { return color; }
	const auto passes = [&backgrounds, minimum]( const QColor& candidate ) {
		return std::all_of( backgrounds.constBegin(), backgrounds.constEnd(),
			[candidate, minimum]( const QColor& background ) {
				return Theme::contrastRatio( candidate, background ) + 0.0001 >= minimum;
			} );
	};
	if( passes( color ) ) { return color; }

	// Search both monotonic HSL directions instead of alternating between them,
	// then choose the closest lightness satisfying every supplied background.
	const auto originalLightness = color.lightness();
	auto result = color;
	int bestDistance = ( std::numeric_limits<int>::max )();
	for( int lightness = 0; lightness <= 255; ++lightness )
	{
		const auto candidate = withLightness( color, lightness );
		const auto distance = std::abs( lightness - originalLightness );
		if( passes( candidate ) && distance < bestDistance )
		{
			result = candidate;
			bestDistance = distance;
		}
	}
	return result;
}


double Theme::contrastRatio( const QColor& foreground, const QColor& background )
{
	const auto luminance = []( const QColor& color ) {
		return 0.2126 * linearChannel( color.red() ) +
			   0.7152 * linearChannel( color.green() ) +
			   0.0722 * linearChannel( color.blue() );
	};
	const auto first = luminance( foreground );
	const auto second = luminance( background );
	return ( std::max( first, second ) + 0.05 ) / ( std::min( first, second ) + 0.05 );
}


QColor Theme::color( ColorRole role )
{
	const auto dark = QGuiApplication::palette().color( QPalette::Window ).lightness() < 128;
	const QColor background = dark ? QColor( "#181818" ) : QColor( "#fff9e8" );
	const QColor surface = dark ? QColor( "#202020" ) : QColor( "#ffffff" );
	const QColor alternate = dark ? QColor( "#2b2b2b" ) : QColor( "#fff3cd" );
	const QVector<QColor> contentBackgrounds{ background, surface, alternate };
	const QColor primaryText( "#4d3b2f" );
	switch( role )
	{
	case ColorRole::Background: return background;
	case ColorRole::Surface: return surface;
	case ColorRole::SurfaceAlternate: return alternate;
	case ColorRole::Text: return dark ? QColor( "#f5f5f5" ) : QColor( "#4d3b2f" );
	case ColorRole::Muted:
		return accessibleColor( dark ? QColor( "#cfcfcf" ) : QColor( "#8a7568" ), contentBackgrounds, 4.5 );
	case ColorRole::Border:
		return accessibleColor( dark ? QColor( "#6f6f6f" ) : QColor( "#b99b63" ), contentBackgrounds, 3.0 );
	case ColorRole::Primary:
		// Primary is also used as word-cloud text on content surfaces. Button
		// fills are derived separately in apply() for their dark labels.
		return accessibleColor( QColor( "#ffb300" ), contentBackgrounds, 4.5 );
	case ColorRole::PrimaryHover:
		return accessibleColor( QColor( "#ffca28" ), { primaryText }, 4.5 );
	case ColorRole::Secondary:
		return accessibleColor( QColor( "#81d4fa" ), contentBackgrounds, 4.5 );
	case ColorRole::Accent:
		return accessibleColor( QColor( "#f48fb1" ), contentBackgrounds, 4.5 );
	case ColorRole::Success:
	case ColorRole::Warning:
		return accessibleColor( role == ColorRole::Success ? QColor( "#66bb6a" ) : QColor( "#ffa726" ),
							   contentBackgrounds, 4.5 );
	case ColorRole::Danger:
		return accessibleColor( QColor( "#ef5350" ), contentBackgrounds, 4.5 );
	case ColorRole::DangerFill:
		// A dark fill is required for its white label; no single color can
		// simultaneously reach 4.5 against both white and our darkest surface.
		return accessibleColor( QColor( "#ef5350" ), { QColor( "#ffffff" ) }, 4.5 );
	case ColorRole::Disabled: return dark ? QColor( "#8b8b8b" ) : QColor( "#77716b" );
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
	const auto contentBackgrounds = QVector<QColor>{
		color( ColorRole::Background ), color( ColorRole::Surface ), color( ColorRole::SurfaceAlternate ) };
	const QColor primaryText( "#4d3b2f" );
	const QColor secondaryText( "#21465b" );
	const auto primaryFill = accessibleColor( QColor( "#ffb300" ), { primaryText }, 4.5 );
	const auto accentFill = accessibleColor( QColor( "#f48fb1" ), { primaryText }, 4.5 );
	const auto secondaryFill = accessibleColor( QColor( "#81d4fa" ), { secondaryText }, 4.5 );
	// Split shared tokens where text and fill need opposite contrast directions.
	sheet.replace( QStringLiteral("background: %ACCENT%;"),
				   QStringLiteral("background: %1;").arg( accentFill.name() ) );
	sheet.replace( QStringLiteral("background: %DANGER%;"),
				   QStringLiteral("background: %1;").arg( color( ColorRole::DangerFill ).name() ) );
	sheet.replace( QStringLiteral("selection-background-color: %SECONDARY%;"),
				   QStringLiteral("selection-background-color: %1; selection-color: %2;")
					   .arg( secondaryFill.name(), secondaryText.name() ) );
	// Primary is a focus border as well as a fill. Darken/lighten the border
	// until it is visible on every content surface.
	const auto primaryBorder = accessibleColor( primaryFill, contentBackgrounds, 3.0 );
	sheet.replace( QStringLiteral("border: 2px solid %PRIMARY%;"),
				   QStringLiteral("border: 2px solid %1;").arg( primaryBorder.name() ) );
	replace( QStringLiteral("%PRIMARY%"), primaryFill );
	replace( QStringLiteral("%PRIMARY_HOVER%"), color( ColorRole::PrimaryHover ) );
	replace( QStringLiteral("%SECONDARY%"), secondaryFill );
	replace( QStringLiteral("%ACCENT%"), accentFill );
	replace( QStringLiteral("%SUCCESS%"), color( ColorRole::Success ) );
	replace( QStringLiteral("%WARNING%"), color( ColorRole::Warning ) );
	replace( QStringLiteral("%DANGER%"), color( ColorRole::Danger ) );
	replace( QStringLiteral("%DISABLED%"), color( ColorRole::Disabled ) );
	widget->setStyleSheet( sheet );
}

}
