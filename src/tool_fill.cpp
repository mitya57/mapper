/*
 *    Copyright 2013 Thomas Schöps
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "tool_fill.h"

#include <limits>

#include <QMessageBox>
#include <QPainter>

#include "map_widget.h"
#include "object.h"
#include "tool_helpers.h"
#include "map_undo.h"
#include "symbol_dock_widget.h"


FillTool::FillTool(MapEditorController* editor, QAction* tool_button, SymbolWidget* symbol_widget)
 : MapEditorToolBase(QCursor(QPixmap(":/images/cursor-fill.png"), 11, 11), Other, editor, tool_button)
{
	this->symbol_widget = symbol_widget;
	
	selectedSymbolsChanged();
	connect(symbol_widget, SIGNAL(selectedSymbolsChanged()), this, SLOT(selectedSymbolsChanged()));
	connect(map(), SIGNAL(symbolChanged(int,Symbol*,Symbol*)), this, SLOT(symbolChanged(int,Symbol*,Symbol*)));
	connect(map(), SIGNAL(symbolDeleted(int,Symbol*)), this, SLOT(symbolDeleted(int,Symbol*)));
}

FillTool::~FillTool()
{
	// Nothing, not inlined
}

// --- NOTE: start of copied section from DrawLineAndAreaTool. ---
// TODO: create a way for tools to specify which symbols / selections they support and deactivate them automatically if these conditions are not satisfied anymore!
void FillTool::selectedSymbolsChanged()
{
	Symbol* symbol = symbol_widget->getSingleSelectedSymbol();
	if (symbol == NULL || ((symbol->getType() & (Symbol::Line | Symbol::Area | Symbol::Combined)) == 0) || symbol->isHidden())
	{
		if (symbol && symbol->isHidden())
			deactivate();
		else
			switchToDefaultDrawTool(symbol);
		return;
	}
	
	last_used_symbol = symbol;
}

void FillTool::symbolChanged(int pos, Symbol* new_symbol, Symbol* old_symbol)
{
	if (old_symbol == last_used_symbol)
		selectedSymbolsChanged();
}

void FillTool::symbolDeleted(int pos, Symbol* old_symbol)
{
	if (old_symbol == last_used_symbol)
		deactivate();
}
// --- NOTE: end copied section ---

void FillTool::clickPress()
{
	const float extent_area_warning_threshold = 600 * 600; // 60 cm x 60 cm
	
	// Get desired extent and warn if it is large
	QRectF map_extent = map()->calculateExtent(true, false);
	if (map_extent.width() * map_extent.height() > extent_area_warning_threshold)
	{
		if (QMessageBox::question(
			window(),
			tr("Warning"),
			tr("The map area is large. Use of the fill tool may be very slow. Do you want to use it anyway?"),
			QMessageBox::No | QMessageBox::Yes) == QMessageBox::No)
		{
			return;
		}
	}
	
	// Rasterize map into image
	QTransform transform;
	QImage image = rasterizeMap(map_extent, transform);
	
	// Calculate click position in image and check if it is inside the map area and free
	QPoint clicked_pixel = transform.map(cur_map_widget->viewportToMapF(click_pos).toQPointF()).toPoint();
	if (!image.rect().contains(clicked_pixel, true))
	{
		QMessageBox::warning(
			window(),
			tr("Error"),
			tr("The clicked area is not bounded, cannot fill this area.")
		);
		return;
	}
	if (qAlpha(image.pixel(clicked_pixel)) > 0)
	{
		QMessageBox::warning(
			window(),
			tr("Error"),
			tr("The clicked position is not free, cannot use the fill tool there.")
		);
		return;
	}
	
	// Go to the right and find collisions with objects.
	// For every collision, trace the boundary of the collision object
	// and check whether the click position is inside the boundary.
	// If it is, the correct outline was found which is then filled.
	for (QPoint start_pixel = clicked_pixel; start_pixel.x() < image.width() - 1; start_pixel += QPoint(1, 0))
	{
		// Check if there is a collision to the right
		QPoint test_pixel = start_pixel + QPoint(1, 0);
		if (qAlpha(image.pixel(test_pixel)) == 0)
			continue;
		
		// Found a collision, trace outline of hit object
		// and check whether the outline contains start_pixel
		std::vector<QPoint> boundary;
		if (!traceBoundary(image, start_pixel, test_pixel, boundary))
		{
			// The outline does not contain start_pixel.
			// Skip over the floating object.
			start_pixel += QPoint(1, 0);
			while (start_pixel.x() < image.width() - 1
				&& qAlpha(image.pixel(start_pixel)) > 0)
				start_pixel += QPoint(1, 0);
			start_pixel -= QPoint(1, 0);
			continue;
		}
		
		// Create fill object
		if (!fillBoundary(boundary, transform.inverted()))
		{
			QMessageBox::warning(
				window(),
				tr("Error"),
				tr("Failed to create the fill object.")
			);
		}
		return;
	}
	
	QMessageBox::warning(
		window(),
		tr("Error"),
		tr("The clicked area is not bounded, cannot fill this area.")
	);
}

void FillTool::updateStatusText()
{
	setStatusBarText(tr("<b>Click</b>: Fill area with active symbol. "));
}

void FillTool::objectSelectionChangedImpl()
{
}

QImage FillTool::rasterizeMap(const QRectF& extent, QTransform& out_transform)
{
	// Draw map into a QImage with the following settings:
	// - specific zoom factor (resolution)
	// - no antialiasing
	// - encode object ids in object colors
	// - draw centerlines in addition to normal rendering
	// TODO: replace prototype implementation
	
	const float zoom_level = 4;
	
	// Create map view centered on the extent
	MapView* view = new MapView(map());
	view->setPositionX(extent.center().x() * 1000);
	view->setPositionY(extent.center().y() * 1000);
	view->setZoom(zoom_level);
	
	// Allocate the image
	QRect image_size = view->calculateViewBoundingBox(extent).toAlignedRect();
	QImage image = QImage(image_size.size(), QImage::Format_ARGB32_Premultiplied);
	
	// Start drawing
	QPainter painter;
	painter.begin(&image);
	
	// Make image transparent
	QPainter::CompositionMode mode = painter.compositionMode();
	painter.setCompositionMode(QPainter::CompositionMode_Clear);
	painter.fillRect(0, 0, image_size.width(), image_size.height(), Qt::transparent);
	painter.setCompositionMode(mode);
	
	// Draw map
	painter.translate(image_size.width() / 2.0, image_size.height() / 2.0);
	view->applyTransform(&painter);
	map()->draw(&painter, extent, true, view->calculateFinalZoomFactor(), true, true);
	
	out_transform = painter.combinedTransform();
	painter.end();
	delete view;
	return image;
}

bool FillTool::traceBoundary(QImage image, QPoint start_pixel, QPoint test_pixel, std::vector< QPoint >& out_boundary)
{
	out_boundary.clear();
	out_boundary.reserve(4096);
	out_boundary.push_back(test_pixel);
	assert(qAlpha(image.pixel(start_pixel)) == 0);
	assert(qAlpha(image.pixel(test_pixel)) > 0);
	
	// Uncomment this and below references to debugImage to generate path visualizations
// 	QImage debugImage = image.copy();
// 	debugImage.setPixel(test_pixel, qRgb(255, 0, 0));
	
	// Go along obstructed pixels with a "right hand on the wall" method.
	// Iteration keeps the following variables as state:
	// cur_pixel: current (obstructed) position
	// fwd_vector: vector from test_pixel to free spot
	QPoint cur_pixel = test_pixel;
	QPoint fwd_vector = start_pixel - test_pixel;
	int max_length = image.width() * image.height();
	for (int i = 0; i < max_length; ++i)
	{
		QPoint right_vector = QPoint(fwd_vector.y(), -fwd_vector.x());
		if (!image.rect().contains(cur_pixel + fwd_vector + right_vector, true))
			return false;
		if (!image.rect().contains(cur_pixel + right_vector, true))
			return false;
		
		if (qAlpha(image.pixel(cur_pixel + fwd_vector + right_vector)) > 0)
		{
			cur_pixel = cur_pixel + fwd_vector + right_vector;
			fwd_vector = -1 * right_vector;
		}
		else if (qAlpha(image.pixel(cur_pixel + right_vector)) > 0)
		{
			cur_pixel = cur_pixel + right_vector;
			// fwd_vector stays the same
		}
		else
		{
			// cur_pixel stays the same
			fwd_vector = right_vector;
		}
		
		QPoint cur_free_pixel = cur_pixel + fwd_vector;
		if (cur_pixel == test_pixel && cur_free_pixel == start_pixel)
			break;
		
// 		debugImage.setPixel(cur_pixel, qRgb(0, 0, 255));
		
		if (out_boundary.back() != cur_pixel)
			out_boundary.push_back(cur_pixel);
	}
	
// 	debugImage.save("debugImage.png");
	
	bool inside = false;
	int size = (int)out_boundary.size();
	int i, j;
	for (i = 0, j = size - 1; i < size; j = i++)
	{
		if ( ((out_boundary[i].y() > start_pixel.y()) != (out_boundary[j].y() > start_pixel.y())) &&
			(start_pixel.x() < (out_boundary[j].x() - out_boundary[i].x()) *
			(start_pixel.y() - out_boundary[i].y()) / (out_boundary[j].y() - out_boundary[i].y()) + out_boundary[i].x()) )
			inside = !inside;
	}
	return inside;
}

bool FillTool::fillBoundary(const std::vector< QPoint >& boundary, QTransform image_to_map)
{
	// Create PathSection vector
	std::vector< PathSection > sections;
	SnappingToolHelper snap_helper(map(), SnappingToolHelper::ObjectPaths);
	SnappingToolHelperSnapInfo snap_info;
	for (size_t b = 0, end = boundary.size(); b < end; ++b)
	{
		MapCoordF map_pos = MapCoordF(image_to_map.map(QPointF(boundary[b])));
		snap_helper.snapToObject(map_pos, cur_map_widget, &snap_info, NULL, std::numeric_limits<float>::max());
		if (snap_info.type != SnappingToolHelper::ObjectPaths)
			continue;
		
		// Insert snap info into sections vector.
		// Start new section if this is the first section,
		// if the object changed,
		// if the clen advancing direction changes,
		// or if the clen advancement is more than a magic factor times the pixel advancement
		bool start_new_section =
			sections.empty()
			|| sections.back().object != snap_info.object
			|| (sections.back().end_clen - sections.back().start_clen) * (snap_info.path_coord.clen - sections.back().end_clen) < 0
			|| qAbs(snap_info.path_coord.clen - sections.back().end_clen) > 5 * (map_pos.lengthTo(MapCoordF(image_to_map.map(QPointF(boundary[b - 1])))));
		
		if (start_new_section)
		{
			PathSection new_section;
			new_section.object = snap_info.object->asPath();
			new_section.start_clen = snap_info.path_coord.clen;
			new_section.end_clen = snap_info.path_coord.clen;
			sections.push_back(new_section);
		}
		else
		{
			sections.back().end_clen = snap_info.path_coord.clen;
		}
	}
	
	// Clean up PathSection vector
	const float pixel_length = (image_to_map.map(QPointF(0, 0)) - image_to_map.map(QPointF(1, 0))).manhattanLength();
	for (int s = 0, end = (int)sections.size(); s < end; ++s)
	{
		PathSection& section = sections[s];
		
		// Remove back-and-forth sections
		if (s > 0)
		{
			PathSection& prev_section = sections[s - 1];
			if (section.object == prev_section.object
				&& qAbs(section.start_clen - prev_section.end_clen) < 2 * pixel_length
				&& (section.end_clen - section.start_clen) * (prev_section.end_clen - prev_section.start_clen) < 0)
			{
				if ((section.end_clen > section.start_clen) == (section.end_clen > prev_section.end_clen))
				{
					// section.end_clen is between prev_section.start_clen and prev_section.end_clen.
					// Delete the new section and shrink prev_section.
					prev_section.end_clen = section.end_clen;
					sections.erase(sections.begin() + s);
				}
				else
				{
					// section.end_clen extends over prev_section.start_clen.
					// Delete prev_section and shrink the new section.
					section.start_clen = prev_section.start_clen;
					sections.erase(sections.begin() + (s - 1));
				}
				--end;
				--s;
			}
		}
		
		// Slightly extend sections where the start is equal to the end,
		// otherwise changePathBounds() will give us the whole path later
		const float epsilon = 1e-4f;
		if (section.end_clen == section.start_clen)
			section.end_clen += epsilon;
	}
	
	// Create fill object
	PathObject* path = new PathObject(last_used_symbol);
	for (size_t s = 0, end = sections.size(); s < end; ++s)
	{
		PathSection& section = sections[s];
		if (section.start_clen > section.object->getPart(0).getLength()
			|| section.end_clen > section.object->getPart(0).getLength())
			continue;
		
		PathObject* part_copy = section.object->duplicatePart(0);
		bool reverse = section.end_clen < section.start_clen;
		part_copy->changePathBounds(0, reverse ? section.end_clen : section.start_clen, reverse ? section.start_clen : section.end_clen);
		if (reverse)
			part_copy->reverse();
		
		if (path->getCoordinateCount() == 0)
			path->appendPath(part_copy);
		else
			path->connectPathParts(0, part_copy, 0, false, false);
		
		delete part_copy;
	}
	if (path->getCoordinateCount() < 2)
	{
		delete path;
		return false;
	}
	path->closeAllParts();
	
	int index = map()->addObject(path);
	map()->clearObjectSelection(false);
	map()->addObjectToSelection(path, true);
	
	DeleteObjectsUndoStep* undo_step = new DeleteObjectsUndoStep(map());
	undo_step->addObject(index);
	map()->objectUndoManager().addNewUndoStep(undo_step);
	
	map()->setObjectsDirty();
	
	return true;
}