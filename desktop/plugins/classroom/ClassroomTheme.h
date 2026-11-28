#pragma once

#include <QColor>
#include <QVector>

class QWidget;

namespace Classroom
{

class Theme
{
public:
	enum class ColorRole
	{
		Background,
		Surface,
		SurfaceAlternate,
		Text,
		Muted,
		Border,
		Primary,
		PrimaryHover,
		Secondary,
		Accent,
		Success,
		Warning,
		Danger,
		DangerFill,
		Disabled
	};

	static void apply( QWidget* widget );
	static QColor color( ColorRole role );
	//! WCAG 2.x relative-luminance contrast ratio (1.0 through 21.0).
	static double contrastRatio( const QColor& foreground, const QColor& background );
	//! Adjust HSL lightness until every intended background meets `minimum`.
	static QColor accessibleColor( const QColor& color, const QVector<QColor>& backgrounds, double minimum );
};

}
