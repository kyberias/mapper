/*
 *    Copyright 2012-2014 Thomas Schöps
 *    Copyright 2013-2015 Kai Pastor
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


#include "map.h"

#include <algorithm>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <qmath.h>
#include <QMessageBox>
#include <QPainter>
#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/georeferencing.h"
#include "core/map_color.h"
#include "core/map_printer.h"
#include "core/map_view.h"
#include "file_format_ocad8.h"
#include "file_format_registry.h"
#include "file_import_export.h"
#include "map_editor.h"
#include "map_part.h"
#include "object_undo.h"
#include "map_widget.h"
#include "object.h"
#include "object_operations.h"
#include "renderable.h"
#include "symbol.h"
#include "symbol_combined.h"
#include "symbol_line.h"
#include "symbol_point.h"
#include "symbol_text.h"
#include "template.h"
#include "undo_manager.h"
#include "util.h"

// ### Misc ###

namespace
{

/** A record of information about the mapping of a color in a source MapColorSet
 *  to a color in a destination MapColorSet.
 */
struct MapColorSetMergeItem
{
	MapColor* src_color;
	MapColor* dest_color;
	std::size_t dest_index;
	std::size_t lower_bound;
	std::size_t upper_bound;
	int lower_errors;
	int upper_errors;
	bool filter;
	
	MapColorSetMergeItem()
	 : src_color(nullptr),
	   dest_color(nullptr),
	   dest_index(0),
	   lower_bound(0),
	   upper_bound(0),
	   lower_errors(0),
	   upper_errors(0),
	   filter(false)
	{ }
};

/** The mapping of all colors in a source MapColorSet
 *  to colors in a destination MapColorSet. */
typedef std::vector<MapColorSetMergeItem> MapColorSetMergeList;

} // namespace



// ### MapColorSet ###

Map::MapColorSet::MapColorSet()
{
	// nothing
}

Map::MapColorSet::~MapColorSet()
{
	for (MapColor* color : colors)
		delete color;
}

void Map::MapColorSet::insert(int pos, MapColor* color)
{
	colors.insert(colors.begin() + pos, color);
	adjustColorPriorities(pos + 1, colors.size());
}

void Map::MapColorSet::erase(int pos)
{
	colors.erase(colors.begin() + pos);
	adjustColorPriorities(pos, colors.size());	
}

void Map::MapColorSet::adjustColorPriorities(int first, int last)
{
	// TODO: delete or update RenderStates with these colors
	for (int i = first; i < last; ++i)
		colors[i]->setPriority(i);
}

// This algorithm tries to maintain the relative order of colors.
MapColorMap Map::MapColorSet::importSet(const Map::MapColorSet& other, std::vector< bool >* filter, Map* map)
{
	MapColorMap out_pointermap;
	
	// Determine number of colors to import
	std::size_t import_count = other.colors.size();
	if (filter)
	{
		Q_ASSERT(filter->size() == other.colors.size());
		
		for (std::size_t i = 0, end = other.colors.size(); i != end; ++i)
		{
			MapColor* color = other.colors[i];
			if (!(*filter)[i] || out_pointermap.contains(color))
			{
				continue;
			}
			
			out_pointermap[color] = nullptr; // temporary used as a flag
			
			// Determine referenced spot colors, and add them to the filter
			if (color->getSpotColorMethod() == MapColor::CustomColor)
			{
				for (const SpotColorComponent& component : color->getComponents())
				{
					if (!out_pointermap.contains(component.spot_color))
					{
						// Add this spot color to the filter
						int j = 0;
						while (other.colors[j] != component.spot_color)
							++j;
						(*filter)[j] = true;
						out_pointermap[component.spot_color] = nullptr;
					}
				}
			}
		}
		import_count = out_pointermap.size();
		out_pointermap.clear();
	}
	
	if (import_count > 0)
	{
		colors.reserve(colors.size() + import_count);
		
		MapColorSetMergeList merge_list;
		merge_list.resize(other.colors.size());
		
		bool priorities_changed = false;
		
		// Initialize merge_list
		MapColorSetMergeList::iterator merge_list_item = merge_list.begin();
		for (std::size_t i = 0; i < other.colors.size(); ++i)
		{
			merge_list_item->filter = (!filter || (*filter)[i]);
			
			MapColor* src_color = other.colors[i];
			merge_list_item->src_color = src_color;
			for (std::size_t k = 0, colors_size = colors.size(); k < colors_size; ++k)
			{
				if (colors[k]->equals(*src_color, false))
				{
					merge_list_item->dest_color = colors[k];
					merge_list_item->dest_index = k;
					out_pointermap[src_color] = colors[k];
					// Prefer a matching color at the same priority,
					// so just abort early if priority matches
					if (merge_list_item->dest_color->getPriority() == merge_list_item->src_color->getPriority())
						break;
				}
			}
			++merge_list_item;
		}
		Q_ASSERT(merge_list_item == merge_list.end());
		
		size_t iteration_number = 1;
		while (true)
		{
			// Evaluate bounds and conflicting order of colors
			int max_conflict_reduction = 0;
			MapColorSetMergeList::iterator selected_item = merge_list.end();
			for (merge_list_item = merge_list.begin(); merge_list_item != merge_list.end(); ++merge_list_item)
			{
				// Check all lower colors for a higher dest_index
				std::size_t& lower_bound(merge_list_item->lower_bound);
				lower_bound = merge_list_item->dest_color ? merge_list_item->dest_index : 0;
				MapColorSetMergeList::iterator it = merge_list.begin();
				for (; it != merge_list_item; ++it)
				{
					if (it->dest_color)
					{
						if (it->dest_index > lower_bound)
						{
							lower_bound = it->dest_index;
						}
						if (merge_list_item->dest_color && merge_list_item->dest_index < it->dest_index)
						{
							++merge_list_item->lower_errors;
						}
					}
				}
				
				// Check all higher colors for a lower dest_index
				std::size_t& upper_bound(merge_list_item->upper_bound);
				upper_bound = merge_list_item->dest_color ? merge_list_item->dest_index : colors.size();
				for (++it; it != merge_list.end(); ++it)
				{
					if (it->dest_color)
					{
						if (it->dest_index < upper_bound)
						{
							upper_bound = it->dest_index;
						}
						if (merge_list_item->dest_color && merge_list_item->dest_index > it->dest_index)
						{
							++merge_list_item->upper_errors;
						}
					}
				}
				
				if (merge_list_item->filter)
				{
					if (merge_list_item->lower_errors == 0 && merge_list_item->upper_errors > max_conflict_reduction)
					{
						int conflict_reduction = merge_list_item->upper_errors;
						// Check new conflicts with insertion index: selected_item->upper_bound
						for (it = merge_list.begin(); it != merge_list_item; ++it)
						{
							if (it->dest_color && selected_item->upper_bound <= it->dest_index)
								--conflict_reduction;
						}
						// Also allow = here to make two-step resolves work
						if (conflict_reduction >= max_conflict_reduction)
						{
							selected_item = merge_list_item;
							max_conflict_reduction = conflict_reduction;
						}
					}
					else if (merge_list_item->upper_errors == 0 && merge_list_item->lower_errors > max_conflict_reduction)
					{
						int conflict_reduction = merge_list_item->lower_errors;
						// Check new conflicts with insertion index: (selected_item->lower_bound+1)
						it = merge_list_item;
						for (++it; it != merge_list.end(); ++it)
						{
							if (it->dest_color && (selected_item->lower_bound+1) > it->dest_index)
								--conflict_reduction;
						}
						// Also allow = here to make two-step resolves work
						if (conflict_reduction >= max_conflict_reduction)
						{
							selected_item = merge_list_item;
							max_conflict_reduction = conflict_reduction;
						}
					}
				}
			}
			
			// Abort if no conflicts or maximum iteration count reached.
			// The latter condition is just to prevent endless loops in
			// case of bugs and should not occur theoretically.
			if (selected_item == merge_list.end() ||
				iteration_number > merge_list.size())
				break;
			
			// Solve selected conflict item
			MapColor* new_color = new MapColor(*selected_item->dest_color);
			selected_item->dest_color = new_color;
			out_pointermap[selected_item->src_color] = new_color;
			std::size_t insertion_index = (selected_item->lower_errors == 0) ? selected_item->upper_bound : (selected_item->lower_bound+1);
			
			if (map)
				map->addColor(new_color, insertion_index);
			else
				colors.insert(colors.begin() + insertion_index, new_color);
			priorities_changed = true;
			
			for (merge_list_item = merge_list.begin(); merge_list_item != merge_list.end(); ++merge_list_item)
			{
				merge_list_item->lower_errors = 0;
				merge_list_item->upper_errors = 0;
				if (merge_list_item->dest_color && merge_list_item->dest_index >= insertion_index)
					++merge_list_item->dest_index;
			}
			selected_item->dest_index = insertion_index;
			
			++iteration_number;
		}
		
		// Some missing colors may be spot color compositions which can be 
		// resolved to new colors only after all colors have been created.
		// That is why we create all missing colors first.
		for (MapColorSetMergeList::reverse_iterator it = merge_list.rbegin(); it != merge_list.rend(); ++it)
		{
			if (it->filter && !it->dest_color)
			{
				it->dest_color = new MapColor(*it->src_color);
				out_pointermap[it->src_color] = it->dest_color;
			}
			else
			{
				// Existing colors don't need to be touched again.
				it->dest_color = nullptr;
			}
		}
		
		// Now process all new colors for spot color resolution and insertion
		for (MapColorSetMergeList::reverse_iterator it = merge_list.rbegin(); it != merge_list.rend(); ++it)
		{
			MapColor* new_color = it->dest_color;
			if (new_color)
			{
				if (new_color->getSpotColorMethod() == MapColor::CustomColor)
				{
					SpotColorComponents components = new_color->getComponents();
					for (SpotColorComponent& component : components)
					{
						Q_ASSERT(out_pointermap.contains(component.spot_color));
						component.spot_color = out_pointermap[component.spot_color];
					}
					new_color->setSpotColorComposition(components);
				}
				
				std::size_t insertion_index = it->upper_bound;
				if (map)
					map->addColor(new_color, insertion_index);
				else
					colors.insert(colors.begin() + insertion_index, new_color);
				priorities_changed = true;
			}
		}
		
		if (map && priorities_changed)
		{
			map->updateAllObjects();
		}
	}
	
	return out_pointermap;
}



// ### Map ###

bool Map::static_initialized = false;
MapColor Map::covering_white(MapColor::CoveringWhite);
MapColor Map::covering_red(MapColor::CoveringRed);
MapColor Map::undefined_symbol_color(MapColor::Undefined);
MapColor Map::registration_color(MapColor::Registration);
LineSymbol* Map::covering_white_line;
LineSymbol* Map::covering_red_line;
LineSymbol* Map::undefined_line;
PointSymbol* Map::undefined_point;
TextSymbol* Map::undefined_text;
CombinedSymbol* Map::covering_combined_line;

Map::Map()
 : color_set()
 , has_spot_colors(false)
 , undo_manager(new UndoManager(this))
 , renderables(new MapRenderables(this))
 , selection_renderables(new MapRenderables(this))
 , renderable_options(Symbol::RenderNormal)
 , printer_config(nullptr)
{
	if (!static_initialized)
		initStatic();
	
	georeferencing.reset(new Georeferencing());
	init();
	
	connect(this, &Map::colorAdded, this, &Map::checkSpotColorPresence);
	connect(this, &Map::colorChanged, this, &Map::checkSpotColorPresence);
	connect(this, &Map::colorDeleted, this, &Map::checkSpotColorPresence);
	connect(undo_manager.data(), &UndoManager::cleanChanged, this, &Map::undoCleanChanged);
}

Map::~Map()
{
	clear();
}

void Map::clear()
{
	for (Symbol* symbol : symbols)
		delete symbol;
	
	for (Template* temp : templates)
		delete temp;
	
	for (Template* temp : closed_templates)
		delete temp;
	
	for (MapPart* part : parts)
		delete part;
}

void Map::init()
{
	color_set = new MapColorSet();
	
	symbols.clear();
	templates.clear();
	closed_templates.clear();
	parts.clear();
	
	first_front_template = 0;
	
	parts.push_back(new MapPart(tr("default part"), this));
	current_part_index = 0;
	
	object_selection.clear();
	first_selected_object = nullptr;
	
	widgets.clear();
	
	undo_manager->clear();
	undo_manager->setClean();
	
	map_notes = QString();
	
	printer_config.reset();
	
	image_template_use_meters_per_pixel = true;
	image_template_meters_per_pixel = 0;
	image_template_dpi = 0;
	image_template_scale = 0;
	
	colors_dirty = false;
	symbols_dirty = false;
	templates_dirty = false;
	objects_dirty = false;
	other_dirty = false;
	unsaved_changes = false;
}

void Map::reset()
{
	clear();
	init();
}

void Map::setScaleDenominator(unsigned int value)
{
	georeferencing->setScaleDenominator(value);
}

unsigned int Map::getScaleDenominator() const
{
	return georeferencing->getScaleDenominator();
}

void Map::changeScale(unsigned int new_scale_denominator, const MapCoord& scaling_center, bool scale_symbols, bool scale_objects, bool scale_georeferencing, bool scale_templates)
{
	if (new_scale_denominator == getScaleDenominator())
		return;
	
	double factor = getScaleDenominator() / (double)new_scale_denominator;
	
	if (scale_symbols)
		scaleAllSymbols(factor);
	if (scale_objects)
	{
		undo_manager->clear();
		scaleAllObjects(factor, scaling_center);
	}
	if (scale_georeferencing)
		georeferencing->setMapRefPoint(scaling_center + factor * (georeferencing->getMapRefPoint() - scaling_center));
	if (scale_templates)
	{
		for (int i = 0; i < getNumTemplates(); ++i)
		{
			Template* temp = getTemplate(i);
			if (temp->isTemplateGeoreferenced())
				continue;
			setTemplateAreaDirty(i);
			temp->scale(factor, scaling_center);
			setTemplateAreaDirty(i);
		}
		for (int i = 0; i < getNumClosedTemplates(); ++i)
		{
			Template* temp = getClosedTemplate(i);
			if (temp->isTemplateGeoreferenced())
				continue;
			temp->scale(factor, scaling_center);
		}
	}
	
	setScaleDenominator(new_scale_denominator);
	setOtherDirty();
	updateAllMapWidgets();
}

void Map::rotateMap(double rotation, const MapCoord& center, bool adjust_georeferencing, bool adjust_declination, bool adjust_templates)
{
	if (fmod(rotation, 2 * M_PI) == 0)
		return;
	
	undo_manager->clear();
	rotateAllObjects(rotation, center);
	
	if (adjust_georeferencing)
	{
		MapCoordF reference_point = MapCoordF(georeferencing->getMapRefPoint() - center);
		reference_point.rotate(-rotation);
		georeferencing->setMapRefPoint(MapCoord(MapCoordF(center) + reference_point));
	}
	if (adjust_declination)
	{
		double rotation_degrees = 180 * rotation / M_PI;
		georeferencing->setDeclination(georeferencing->getDeclination() + rotation_degrees);
	}
	if (adjust_templates)
	{
		for (int i = 0; i < getNumTemplates(); ++i)
		{
			Template* temp = getTemplate(i);
			if (temp->isTemplateGeoreferenced())
				continue;
			setTemplateAreaDirty(i);
			temp->rotate(rotation, center);
			setTemplateAreaDirty(i);
		}
		for (int i = 0; i < getNumClosedTemplates(); ++i)
		{
			Template* temp = getClosedTemplate(i);
			if (temp->isTemplateGeoreferenced())
				continue;
			temp->rotate(rotation, center);
		}
	}
	
	setOtherDirty();
	updateAllMapWidgets();
}

void Map::setMapNotes(const QString& text)
{
	map_notes = text;
}

bool Map::saveTo(const QString& path, MapView* view)
{
	bool success = exportTo(path, view);
	if (success)
	{
		colors_dirty = false;
		symbols_dirty = false;
		templates_dirty = false;
		objects_dirty = false;
		other_dirty = false;
		unsaved_changes = false;
		undoManager().setClean();
	}
	return success;
}

bool Map::exportTo(const QString& path, MapView* view, const FileFormat* format)
{
	Q_ASSERT(view && "Saving a file without view information is not supported!");
	
	if (!format)
		format = FileFormats.findFormatForFilename(path);

	if (!format)
		format = FileFormats.findFormat(FileFormats.defaultFormat());
	
	if (!format)
	{
		QMessageBox::warning(nullptr, tr("Error"), tr("Cannot export the map as\n\"%1\"\nbecause the format is unknown.").arg(path));
		return false;
	}
	else if (!format->supportsExport())
	{
		QMessageBox::warning(nullptr, tr("Error"), tr("Cannot export the map as\n\"%1\"\nbecause saving as %2 (.%3) is not supported.").arg(path).arg(format->description()).arg(format->fileExtensions().join(", ")));
		return false;
	}
	
	// Update the relative paths of templates
	QDir map_dir = QFileInfo(path).absoluteDir();
	for (int i = 0; i < getNumTemplates(); ++i)
	{
		Template* temp = getTemplate(i);
		if (temp->getTemplateState() == Template::Invalid)
			temp->setTemplateRelativePath(QString());
		else
			temp->setTemplateRelativePath(map_dir.relativeFilePath(temp->getTemplatePath()));
	}
	for (int i = 0; i < getNumClosedTemplates(); ++i)
	{
		Template* temp = getClosedTemplate(i);
		if (temp->getTemplateState() == Template::Invalid)
			temp->setTemplateRelativePath(QString());
		else
			temp->setTemplateRelativePath(map_dir.relativeFilePath(temp->getTemplatePath()));
	}
	
	QSaveFile file(path);
	QScopedPointer<Exporter> exporter(format->createExporter(&file, this, view));
	bool success = false;
	if (file.open(QIODevice::WriteOnly))
	{
		try
		{
			exporter->doExport();
		}
		catch (std::exception &e)
		{
			file.cancelWriting();
			const QString error = QString::fromLocal8Bit(e.what());
			QMessageBox::warning(nullptr, tr("Error"), tr("Internal error while saving:\n%1").arg(error));
			return false;
		}
		
		success = file.commit();
	}
	
	if (!success)
	{
		QMessageBox::warning(
		  nullptr,
		  tr("Error"),
		  /** @todo: Switch to the latter when translated. */ true ?
		  tr("Cannot open file:\n%1\n\n%2").arg(path).arg(file.errorString()) :
		  tr("Cannot save file\n%1:\n%2")
		);
	}
	else if (!exporter->warnings().empty())
	{
		QString warnings;
		for (std::vector<QString>::const_iterator it = exporter->warnings().begin(); it != exporter->warnings().end(); ++it) {
			if (!warnings.isEmpty())
				warnings += '\n';
			warnings += *it;
		}
		QMessageBox msgBox(QMessageBox::Warning, tr("Warning"), tr("The map export generated warnings."), QMessageBox::Ok);
		msgBox.setDetailedText(warnings);
		msgBox.exec();
	}
	
	return success;
}

bool Map::loadFrom(const QString& path, QWidget* dialog_parent, MapView* view, bool load_symbols_only, bool show_error_messages)
{
	// Ensure the file exists and is readable.
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		if (show_error_messages)
			QMessageBox::warning(
			  dialog_parent,
			  tr("Error"),
			  tr("Cannot open file:\n%1\nfor reading.").arg(path)
			);
		return false;
	}
	
	// Delete previous objects
	reset();

	// Read a block at the beginning of the file, that we can use for magic number checking.
	unsigned char buffer[256];
	size_t total_read = file.read((char *)buffer, 256);
	file.seek(0);

	bool import_complete = false;
	QString error_msg = tr("Invalid file type.");
	for (auto format : FileFormats.formats())
	{
		// If the format supports import, and thinks it can understand the file header, then proceed.
		if (format->supportsImport() && format->understands(buffer, total_read))
		{
			Importer *importer = nullptr;
			// Wrap everything in a try block, so we can gracefully recover if the importer balks.
			try {
				// Create an importer instance for this file and map.
				importer = format->createImporter(&file, this, view);

				// Run the first pass.
				importer->doImport(load_symbols_only, QFileInfo(path).absolutePath());

				// Are there any actions the user must take to complete the import?
				if (!importer->actions().empty())
				{
					// TODO: prompt the user to resolve the action items. All-in-one dialog.
				}

				// Finish the import.
				importer->finishImport();
				
				file.close();

				// Display any warnings.
				if (!importer->warnings().empty() && show_error_messages)
				{
					QString warnings;
					for (std::vector<QString>::const_iterator it = importer->warnings().begin(); it != importer->warnings().end(); ++it) {
						if (!warnings.isEmpty())
							warnings += '\n';
						warnings += *it;
					}
					QMessageBox msgBox(
					  QMessageBox::Warning,
					  tr("Warning"),
					  tr("The map import generated warnings."),
					  QMessageBox::Ok,
					  dialog_parent
					);
					msgBox.setDetailedText(warnings);
					msgBox.exec();
				}

				import_complete = true;
			}
			catch (std::exception &e)
			{
				qDebug() << "Exception:" << e.what();
				error_msg = e.what();
			}
			if (importer) delete importer;
		}
		// If the last importer finished successfully
		if (import_complete) break;
	}
	
	if (view)
	{
		view->setPanOffset(QPoint(0, 0));
	}

	if (!import_complete)
	{
		if (show_error_messages)
			QMessageBox::warning(
			 dialog_parent,
			 tr("Error"),
			 tr("Cannot open file:\n%1\n\n%2").arg(path).arg(error_msg)
			);
		return false;
	}

	// Update all objects without trying to remove their renderables first, this gives a significant speedup when loading large files
	updateAllObjects(); // TODO: is the comment above still applicable?
	
	setHasUnsavedChanges(false);

	return true;
}

void Map::importMap(Map* other, ImportMode mode, QWidget* dialog_parent, std::vector<bool>* filter, int symbol_insert_pos,
					bool merge_duplicate_symbols, QHash<const Symbol*, Symbol*>* out_symbol_map)
{
	// Check if there is something to import
	if (other->getNumColors() == 0 && other->getNumSymbols() == 0 && other->getNumObjects() == 0)
	{
		QMessageBox::critical(dialog_parent, tr("Error"), tr("Nothing to import."));
		return;
	}
	
	// Check scale
	if (other->getNumSymbols() > 0 && other->getScaleDenominator() != getScaleDenominator())
	{
		int answer = QMessageBox::question(dialog_parent, tr("Question"),
										   tr("The scale of the imported data is 1:%1 which is different from this map's scale of 1:%2.\n\nRescale the imported data?")
										   .arg(QLocale().toString(other->getScaleDenominator()))
										   .arg(QLocale().toString(getScaleDenominator())), QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (answer == QMessageBox::Yes)
			other->changeScale(getScaleDenominator(), MapCoord(0, 0), true, true, true, true);
	}
	
	// TODO: As a special case if both maps are georeferenced, the location of the imported objects could be corrected
	
	// Determine which symbols to import
	std::vector<bool> symbol_filter;
	symbol_filter.resize(other->getNumSymbols(), true);
	if (mode == MinimalObjectImport)
	{
		if (other->getNumObjects() > 0)
			other->determineSymbolsInUse(symbol_filter);
	}
	else if (mode == MinimalSymbolImport && filter)
	{
		Q_ASSERT(filter->size() == symbol_filter.size());
		symbol_filter = *filter;
		other->determineSymbolUseClosure(symbol_filter);
	}
	
	// Determine which colors to import
	std::vector<bool> color_filter;
	color_filter.resize(other->getNumColors(), true);
	if (mode == MinimalObjectImport || mode == MinimalSymbolImport)
	{
		if (other->getNumSymbols() > 0)
			other->determineColorsInUse(symbol_filter, color_filter);
	}
	else if (mode == ColorImport && filter)
	{
		Q_ASSERT(filter->size() == color_filter.size());
		color_filter = *filter;
	}
	
	// Import colors
	MapColorMap color_map(color_set->importSet(*other->color_set, &color_filter, this));
	
	if (mode == ColorImport)
		return;
	
	QHash<const Symbol*, Symbol*> symbol_map;
	if (other->getNumSymbols() > 0)
	{
		// Import symbols
		importSymbols(other, color_map, symbol_insert_pos, merge_duplicate_symbols, &symbol_filter, nullptr, &symbol_map);
		if (out_symbol_map != nullptr)
			*out_symbol_map = symbol_map;
	}
	
	if (mode == MinimalSymbolImport)
		return;
	
	if (other->getNumObjects() > 0)
	{
		// Import parts like this:
		//  - if the other map has only one part, import it into the current part
		//  - else check if there is already a part with an equal name for every part to import and import into this part if found, else create a new part
		for (int part = 0; part < other->getNumParts(); ++part)
		{
			MapPart* part_to_import = other->getPart(part);
			MapPart* dest_part = nullptr;
			if (other->getNumParts() == 1)
				dest_part = getCurrentPart();
			else
			{
				for (int check_part = 0; check_part < getNumParts(); ++check_part)
				{
					if (getPart(check_part)->getName().compare(other->getPart(part)->getName(), Qt::CaseInsensitive) == 0)
					{
						dest_part = getPart(check_part);
						break;
					}
				}
				if (dest_part == nullptr)
				{
					// Import as new part
					dest_part = new MapPart(part_to_import->getName(), this);
					addPart(dest_part, 0);
				}
			}
			
			// Temporarily switch the current part for importing so the undo step gets created for the right part
			MapPart* temp_current_part = getCurrentPart();
			current_part_index = findPartIndex(dest_part);
			
			bool select_and_center_objects = dest_part == temp_current_part;
			dest_part->importPart(part_to_import, symbol_map, select_and_center_objects);
			if (select_and_center_objects)
				ensureVisibilityOfSelectedObjects(Map::FullVisibility);
			
			current_part_index = findPartIndex(temp_current_part);
		}
	}
}

bool Map::exportToIODevice(QIODevice* stream)
{
	stream->open(QIODevice::WriteOnly);
	Exporter* exporter = nullptr;
	try {
		const FileFormat* native_format = FileFormats.findFormat(QStringLiteral("XML"));
		exporter = native_format->createExporter(stream, this, nullptr);
		exporter->doExport();
		stream->close();
	}
	catch (std::exception &e)
	{
		if (exporter)
			delete exporter;
		return false;
	}
	if (exporter)
		delete exporter;
	return true;
}

bool Map::importFromIODevice(QIODevice* stream)
{
	Importer* importer = nullptr;
	try {
		const FileFormat* native_format = FileFormats.findFormat(QStringLiteral("XML"));
		importer = native_format->createImporter(stream, this, nullptr);
		importer->doImport(false);
		importer->finishImport();
		stream->close();
	}
	catch (std::exception &e)
	{
		if (importer)
			delete importer;
		return false;
	}
	if (importer)
		delete importer;
	return true;
}

void Map::draw(QPainter* painter, const RenderConfig& config)
{
	// Update the renderables of all objects marked as dirty
	updateObjects();
	
	// The actual drawing
	renderables->draw(painter, config);
}

void Map::drawOverprintingSimulation(QPainter* painter, const RenderConfig& config)
{
	// Update the renderables of all objects marked as dirty
	updateObjects();
	
	// The actual drawing
	renderables->drawOverprintingSimulation(painter, config);
}

void Map::drawColorSeparation(QPainter* painter, const RenderConfig& config, const MapColor* spot_color, bool use_color)
{
	// Update the renderables of all objects marked as dirty
	updateObjects();
	
	// The actual drawing
	renderables->drawColorSeparation(painter, config, spot_color, use_color);
}

void Map::drawGrid(QPainter* painter, QRectF bounding_box, bool on_screen)
{
	grid.draw(painter, bounding_box, this, on_screen);
}

void Map::drawTemplates(QPainter* painter, QRectF bounding_box, int first_template, int last_template, const MapView* view, bool on_screen) const
{
	for (int i = first_template; i <= last_template; ++i)
	{
		const Template* temp = getTemplate(i);
		bool visible  = temp->getTemplateState() == Template::Loaded;
		double scale  = std::max(temp->getTemplateScaleX(), temp->getTemplateScaleY());
		float opacity = 1.0f;
		if (view)
		{
			const TemplateVisibility* visibility = view->getTemplateVisibility(temp);
			visible &= visibility->visible;
			opacity  = visibility->opacity;
			scale   *= view->getZoom();
		}
		if (visible)
		{
			painter->save();
			temp->drawTemplate(painter, bounding_box, scale, on_screen, opacity);
			painter->restore();
		}
	}
}

void Map::updateObjects()
{
	// TODO: It maybe would be better if the objects entered themselves into a separate list when they get dirty so not all objects have to be traversed here
	applyOnAllObjects(ObjectOp::Update());
}

void Map::removeRenderablesOfObject(const Object* object, bool mark_area_as_dirty)
{
	renderables->removeRenderablesOfObject(object, mark_area_as_dirty);
	if (isObjectSelected(object))
		removeSelectionRenderables(object);
}
void Map::insertRenderablesOfObject(const Object* object)
{
	renderables->insertRenderablesOfObject(object);
	if (isObjectSelected(object))
		addSelectionRenderables(object);
}


void Map::markAsIrregular(Object* object)
{
	irregular_objects.insert(object);
}

const std::set<Object*> Map::irregularObjects() const
{
	return irregular_objects;
}

std::size_t Map::deleteIrregularObjects()
{
	std::size_t result = 0;
	std::set<Object*> unhandled;
	for (auto object : irregular_objects)
	{
		for (auto part : parts)
		{
			if (part->deleteObject(object, false))
			{
				++result;
				goto next_object;
			}
		}
		unhandled.insert(object);
next_object:
		; // nothing else
	}
	
	irregular_objects.swap(unhandled);
	return result;
}


void Map::getSelectionToSymbolCompatibility(const Symbol* symbol, bool& out_compatible, bool& out_different) const
{
	out_compatible = symbol && !object_selection.empty();
	out_different = false;
	
	if (symbol)
	{
		for (const Object* object : object_selection)
		{
			if (!symbol->isTypeCompatibleTo(object))
			{
				out_compatible = false;
				out_different = true;
				return;
			}
			else if (symbol != object->getSymbol())
				out_different = true;
		}
	}
}

void Map::deleteSelectedObjects()
{
	Map::ObjectSelection::const_iterator obj = selectedObjectsBegin();
	Map::ObjectSelection::const_iterator end = selectedObjectsEnd();
	if (obj != end)
	{
		// FIXME: this is not ready for multiple map parts.
		AddObjectsUndoStep* undo_step = new AddObjectsUndoStep(this);
		MapPart* part = getCurrentPart();
	
		for (; obj != end; ++obj)
		{
			int index = part->findObjectIndex(*obj);
			if (index >= 0)
			{
				undo_step->addObject(index, *obj);
				part->deleteObject(index, true);
			}
			else
			{
				qDebug() << this << "::deleteSelectedObjects(): Object" << *obj << "not found in current map part.";
			}
		}
		
		setObjectsDirty();
		clearObjectSelection(true);
		push(undo_step);
	}
}

void Map::includeSelectionRect(QRectF& rect) const
{
	for (const Object* object : object_selection)
		rectIncludeSafe(rect, object->getExtent());
}

void Map::drawSelection(QPainter* painter, bool force_min_size, MapWidget* widget, MapRenderables* replacement_renderables, bool draw_normal)
{
	MapView* view = widget->getMapView();
	
	painter->save();
	painter->translate(widget->width() / 2.0 + view->panOffset().x(), widget->height() / 2.0 + view->panOffset().y());
	painter->setWorldTransform(view->worldTransform(), true);
	
	if (!replacement_renderables)
		replacement_renderables = selection_renderables.data();
	
	RenderConfig::Options options = RenderConfig::Screen | RenderConfig::HelperSymbols;
	qreal selection_opacity = 1.0;
	if (force_min_size)
		options |= RenderConfig::ForceMinSize;
	if (!draw_normal)
	{
		options |= RenderConfig::Highlighted;
		selection_opacity = 0.4;
	}
	RenderConfig config = { *this, view->calculateViewedRect(widget->viewportToView(widget->rect())), view->calculateFinalZoomFactor(), options, selection_opacity };
	replacement_renderables->draw(painter, config);
	
	painter->restore();
}

void Map::addObjectToSelection(Object* object, bool emit_selection_changed)
{
	Q_ASSERT(!isObjectSelected(object));
	object_selection.insert(object);
	addSelectionRenderables(object);
	if (!first_selected_object)
		first_selected_object = object;
	if (emit_selection_changed)
		emit(objectSelectionChanged());
}

void Map::removeObjectFromSelection(Object* object, bool emit_selection_changed)
{
	bool removed = object_selection.erase(object);
	Q_ASSERT(removed && "Map::removeObjectFromSelection: object was not selected!");
	Q_UNUSED(removed);
	removeSelectionRenderables(object);
	if (first_selected_object == object)
		first_selected_object = object_selection.empty() ? nullptr : *object_selection.begin();
	if (emit_selection_changed)
		emit(objectSelectionChanged());
}

bool Map::removeSymbolFromSelection(const Symbol* symbol, bool emit_selection_changed)
{
	bool removed_at_least_one_object = false;
	ObjectSelection::iterator it_end = object_selection.end();
	for (ObjectSelection::iterator it = object_selection.begin(); it != it_end; )
	{
		if ((*it)->getSymbol() != symbol)
		{
			++it;
			continue;
		}
		
		removed_at_least_one_object = true;
		removeSelectionRenderables(*it);
		Object* removed_object = *it;
		it = object_selection.erase(it);
		if (first_selected_object == removed_object)
			first_selected_object = object_selection.empty() ? nullptr : *object_selection.begin();
	}
	if (emit_selection_changed && removed_at_least_one_object)
		emit(objectSelectionChanged());
	return removed_at_least_one_object;
}

bool Map::isObjectSelected(const Object* object) const
{
	return object_selection.find(const_cast<Object*>(object)) != object_selection.end();
}

bool Map::toggleObjectSelection(Object* object, bool emit_selection_changed)
{
	if (isObjectSelected(object))
	{
		removeObjectFromSelection(object, emit_selection_changed);
		return false;
	}
	else
	{
		addObjectToSelection(object, emit_selection_changed);
		return true;
	}
}

void Map::clearObjectSelection(bool emit_selection_changed)
{
	selection_renderables->clear();
	object_selection.clear();
	first_selected_object = nullptr;
	
	if (emit_selection_changed)
		emit(objectSelectionChanged());
}

void Map::emitSelectionChanged()
{
	emit(objectSelectionChanged());
}

void Map::emitSelectionEdited()
{
	emit(selectedObjectEdited());
}

void Map::addMapWidget(MapWidget* widget)
{
	widgets.push_back(widget);
}

void Map::removeMapWidget(MapWidget* widget)
{
	widgets.erase(std::remove(begin(widgets), end(widgets), widget), end(widgets));
}


void Map::updateAllMapWidgets()
{
	for (MapWidget* widget : widgets)
		widget->updateEverything();
}


void Map::ensureVisibilityOfSelectedObjects(SelectionVisibility visibility)
{
	if (!object_selection.empty())
	{
		QRectF rect;
		includeSelectionRect(rect);
		
		for (MapWidget* widget : widgets)
		{
			switch (visibility)
			{
			case FullVisibility:
				widget->ensureVisibilityOfRect(rect, MapWidget::DiscreteZoom);
				break;
				
			case PartialVisibility:
				if (!widget->getMapView()->calculateViewedRect(widget->viewportToView(widget->geometry())).intersects(rect))
					widget->ensureVisibilityOfRect(rect, MapWidget::DiscreteZoom);
				break;
				
			case IgnoreVisibilty:
				break; // Do nothing
				
			default:
				Q_UNREACHABLE();
			}
		}
	}
}


void Map::setDrawingBoundingBox(QRectF map_coords_rect, int pixel_border, bool do_update)
{
	for (MapWidget* widget : widgets)
		widget->setDrawingBoundingBox(map_coords_rect, pixel_border, do_update);
}

void Map::clearDrawingBoundingBox()
{
	for (MapWidget* widget : widgets)
		widget->clearDrawingBoundingBox();
}


void Map::setActivityBoundingBox(QRectF map_coords_rect, int pixel_border, bool do_update)
{
	for (MapWidget* widget : widgets)
		widget->setActivityBoundingBox(map_coords_rect, pixel_border, do_update);
}

void Map::clearActivityBoundingBox()
{
	for (MapWidget* widget : widgets)
		widget->clearActivityBoundingBox();
}


void Map::updateDrawing(QRectF map_coords_rect, int pixel_border)
{
	for (MapWidget* widget : widgets)
		widget->updateDrawing(map_coords_rect, pixel_border);
}

void Map::setColor(MapColor* color, int pos)
{
	// MapColor* old_color = color_set->colors[pos];
	
	color_set->colors[pos] = color;
	color->setPriority(pos);
	
	if (color->getSpotColorMethod() == MapColor::SpotColor)
	{
		// Update dependent colors
		for (MapColor* map_color : color_set->colors)
		{
			if (map_color->getSpotColorMethod() == MapColor::CustomColor)
			{
				for (const SpotColorComponent& component : map_color->getComponents())
				{
					if (component.spot_color == color)
					{
						// Assuming each spot color is rarely used more than once per composition
						if (map_color->getCmykColorMethod() == MapColor::SpotColor)
							map_color->setCmykFromSpotColors();
						if (map_color->getRgbColorMethod() == MapColor::SpotColor)
							map_color->setRgbFromSpotColors();
						updateSymbolIcons(map_color);
						emit colorChanged(map_color->getPriority(), map_color);
					}
				}
			}
		}
	}
	else
	{
		// Remove from dependent colors
		for (MapColor* map_color : color_set->colors)
		{
			if (map_color->getSpotColorMethod() == MapColor::CustomColor
			    && map_color->removeSpotColorComponent(color))
			{
				updateSymbolIcons(map_color);
				emit colorChanged(map_color->getPriority(), map_color);
			}
		}
	}
	
	updateSymbolIcons(color);
	emit(colorChanged(pos, color));
}

void Map::addColor(MapColor* color, int pos)
{
	color_set->insert(pos, color);
	if (getNumColors() == 1)
	{
		// This is the first color - the help text in the map widget(s) should be updated
		updateAllMapWidgets();
	}
	setColorsDirty();
	emit(colorAdded(pos, color));
	color->setPriority(pos);
}

void Map::deleteColor(int pos)
{
	MapColor* color = color_set->colors[pos];
	if (color->getSpotColorMethod() == MapColor::SpotColor)
	{
		// Update dependent colors
		for (MapColor* map_color : color_set->colors)
		{
			if (map_color->removeSpotColorComponent(color))
			{
				updateSymbolIcons(map_color);
				emit colorChanged(map_color->getPriority(), map_color);
			}
		}
	}
	
	color_set->erase(pos);
	
	if (getNumColors() == 0)
	{
		// That was the last color - the help text in the map widget(s) should be updated
		updateAllMapWidgets();
	}
	
	// Treat combined symbols first before their parts
	for (Symbol* symbol : symbols)
	{
		if (symbol->getType() == Symbol::Combined)
			symbol->colorDeleted(color);
	}
	for (Symbol* symbol : symbols)
	{
		if (symbol->getType() != Symbol::Combined)
			symbol->colorDeleted(color);
	}
	emit(colorDeleted(pos, color));
	
	delete color;
}

int Map::findColorIndex(const MapColor* color) const
{
	std::size_t size = color_set->colors.size();
	for (std::size_t i = 0; i < size; ++i)
	{
		if (color_set->colors[i] == color)
			return (int)i;
	}
	if (color && color->getPriority() == MapColor::Registration)
	{
		return MapColor::Registration;
	}
	return -1;
}

void Map::setColorsDirty()
{
	colors_dirty = true;
	setHasUnsavedChanges(true);
}

void Map::useColorsFrom(Map* map)
{
	color_set = map->color_set;
}

bool Map::isColorUsedByASymbol(const MapColor* color) const
{
	for (const Symbol* symbol : symbols)
	{
		if (symbol->containsColor(color))
			return true;
	}
	return false;
}

void Map::determineColorsInUse(const std::vector< bool >& by_which_symbols, std::vector< bool >& out) const
{
	if (getNumSymbols() == 0)
	{
		out.clear();
		return;
	}
	
	Q_ASSERT((int)by_which_symbols.size() == getNumSymbols());
	out.assign(getNumColors(), false);
	for (int c = 0; c < getNumColors(); ++c)
	{
		for (int s = 0; s < getNumSymbols(); ++s)
		{
			if (by_which_symbols[s] && getSymbol(s)->containsColor(getColor(c)))
			{
				out[c] = true;
				break;
			}
		}
	}
}

void Map::checkSpotColorPresence()
{
	const bool has_spot_colors = hasSpotColors();
	if (this->has_spot_colors != has_spot_colors)
	{
		this->has_spot_colors = has_spot_colors;
		emit spotColorPresenceChanged(has_spot_colors);
	}
}

bool Map::hasSpotColors() const
{
	for (const MapColor* color : color_set->colors)
	{
		if (color->getSpotColorMethod() == MapColor::SpotColor)
			return true;
	}
	return false;
}

void Map::importSymbols(Map* other, const MapColorMap& color_map, int insert_pos, bool merge_duplicates, std::vector< bool >* filter,
						QHash< int, int >* out_indexmap, QHash<const Symbol*, Symbol*>* out_pointermap)
{
	// We need a pointer map (and keep track of added symbols) to adjust the references of combined symbols
	std::vector<Symbol*> added_symbols;
	QHash<const Symbol*, Symbol*> local_pointermap;
	if (!out_pointermap)
		out_pointermap = &local_pointermap;
	
	for (size_t i = 0, end = other->symbols.size(); i < end; ++i)
	{
		if (filter && !filter->at(i))
			continue;
		Symbol* other_symbol = other->symbols.at(i);
		
		if (!merge_duplicates)
		{
			added_symbols.push_back(other_symbol);
			continue;
		}
		
		// Check if symbol is already present
		size_t k = 0;
		size_t symbols_size = symbols.size();
		for (; k < symbols_size; ++k)
		{
			if (symbols[k]->equals(other_symbol, Qt::CaseInsensitive, false))
			{
				// Symbol is already present
				if (out_indexmap)
					out_indexmap->insert(i, k);
				if (out_pointermap)
					out_pointermap->insert(other_symbol, symbols[k]);
				break;
			}
		}
		
		if (k >= symbols_size)
		{
			// Symbols does not exist in this map yet, mark it to be added
			added_symbols.push_back(other_symbol);
		}
	}
	
	// Really add all the added symbols
	if (insert_pos < 0)
		insert_pos = getNumSymbols();
	for (size_t i = 0, end = added_symbols.size(); i < end; ++i)
	{
		Symbol* old_symbol = added_symbols[i];
		Symbol* new_symbol = added_symbols[i]->duplicate(&color_map);
		added_symbols[i] = new_symbol;
		
		addSymbol(new_symbol, insert_pos);
		++insert_pos;
		
		if (out_indexmap)
			out_indexmap->insert(i, insert_pos);
		if (out_pointermap)
			out_pointermap->insert(old_symbol, new_symbol);
	}
	
	// Notify added symbols of other "environment"
	for (size_t i = 0, end = added_symbols.size(); i < end; ++i)
	{
		QHash<const Symbol*, Symbol*>::iterator it = out_pointermap->begin();
		while (it != out_pointermap->end())
		{
			added_symbols[i]->symbolChanged(it.key(), it.value());
			++it;
		}
	}
}

void Map::addSelectionRenderables(const Object* object)
{
	object->update();
	selection_renderables->insertRenderablesOfObject(object);
}

void Map::updateSelectionRenderables(const Object* object)
{
	removeSelectionRenderables(object);
	addSelectionRenderables(object);
}

void Map::removeSelectionRenderables(const Object* object)
{
	selection_renderables->removeRenderablesOfObject(object, false);
}

void Map::initStatic()
{
	static_initialized = true;
	
	covering_white_line = new LineSymbol();
	covering_white_line->setColor(&covering_white);
	covering_white_line->setLineWidth(3.0);
	
	covering_red_line = new LineSymbol();
	covering_red_line->setColor(&covering_red);
	covering_red_line->setLineWidth(1.0);
	
	covering_combined_line = new CombinedSymbol();
	covering_combined_line->setNumParts(2);
	covering_combined_line->setPart(0, covering_white_line, false);
	covering_combined_line->setPart(1, covering_red_line, false);
	
	// Undefined symbols
	undefined_line = new LineSymbol();
	undefined_line->setColor(&undefined_symbol_color);
	undefined_line->setLineWidth(1);
	undefined_line->setIsHelperSymbol(true);
	
	undefined_point = new PointSymbol();
	undefined_point->setInnerRadius(100);
	undefined_point->setInnerColor(&undefined_symbol_color);
	undefined_point->setIsHelperSymbol(true);
	
	undefined_text = new TextSymbol();
	undefined_text->setColor(&undefined_symbol_color);
	undefined_text->setIsHelperSymbol(true);
}

void Map::addSymbol(Symbol* symbol, int pos)
{
	symbols.insert(symbols.begin() + pos, symbol);
	if (symbols.size() == 1)
	{
		// This is the first symbol - the help text in the map widget(s) should be updated
		updateAllMapWidgets();
	}
	
	emit(symbolAdded(pos, symbol));
	setSymbolsDirty();
}

void Map::moveSymbol(int from, int to)
{
	symbols.insert(symbols.begin() + to, symbols[from]);
	if (from > to)
		++from;
	symbols.erase(symbols.begin() + from);
	// TODO: emit(symbolChanged(pos, symbol)); ?
	setSymbolsDirty();
}

const Symbol* Map::getSymbol(int i) const
{
	if (i >= 0)
		return symbols[i];
	else if (i == -1)
		return nullptr;
	else if (i == -2)
		return getUndefinedPoint();
	else if (i == -3)
		return getUndefinedLine();
	else
	{
		Q_ASSERT(!"Invalid symbol index given");
		return getUndefinedLine();
	}
}

Symbol* Map::getSymbol(int i)
{
	return const_cast<Symbol*>(static_cast<const Map*>(this)->getSymbol(i));
}

void Map::setSymbol(Symbol* symbol, int pos)
{
	Symbol* old_symbol = symbols[pos];
	
	// Check if an object with this symbol is selected
	bool object_with_symbol_selected = false;
	for (const Object* object : object_selection)
	{
		if (object->getSymbol() == old_symbol || object->getSymbol()->containsSymbol(old_symbol))
		{
			object_with_symbol_selected = true;
			break;
		}
	}
	
	changeSymbolForAllObjects(old_symbol, symbol);
	
	int size = (int)symbols.size();
	for (int i = 0; i < size; ++i)
	{
		if (i == pos)
			continue;
		
		if (symbols[i]->symbolChanged(symbols[pos], symbol))
			updateAllObjectsWithSymbol(symbols[i]);
	}
	
	// Change the symbol
	symbols[pos] = symbol;
	emit(symbolChanged(pos, symbol, old_symbol));
	setSymbolsDirty();
	delete old_symbol;
	
	if (object_with_symbol_selected)
		emit selectedObjectEdited();
}

void Map::deleteSymbol(int pos)
{
	if (deleteAllObjectsWithSymbol(symbols[pos]))
		undo_manager->clear();
	
	int size = (int)symbols.size();
	for (int i = 0; i < size; ++i)
	{
		if (i == pos)
			continue;
		
		if (symbols[i]->symbolChanged(symbols[pos], nullptr))
			updateAllObjectsWithSymbol(symbols[i]);
	}
	
	// Delete the symbol
	Symbol* temp = symbols[pos];
	delete symbols[pos];
	symbols.erase(symbols.begin() + pos);
	
	if (symbols.empty())
	{
		// That was the last symbol - the help text in the map widget(s) should be updated
		updateAllMapWidgets();
	}
	
	emit(symbolDeleted(pos, temp));
	setSymbolsDirty();
}

int Map::findSymbolIndex(const Symbol* symbol) const
{
	if (!symbol)
		return -1;
	int size = (int)symbols.size();
	for (int i = 0; i < size; ++i)
	{
		if (symbols[i] == symbol)
			return i;
	}
	
	if (symbol == undefined_point)
		return -2;
	else if (symbol == undefined_line)
		return -3;
	else if (symbol == undefined_text)
		return -4;
	
	// maybe element of point symbol
	return -1;
}

void Map::setSymbolsDirty()
{
	symbols_dirty = true;
	setHasUnsavedChanges(true);
}

void Map::updateSymbolIcons(const MapColor* color)
{
	for (std::size_t i = 0, size = symbols.size(); i < size; ++i)
	{
		if (symbols[i]->containsColor(color))
		{
			symbols[i]->resetIcon();
			emit(symbolIconChanged(i));
		}
	}
}

void Map::scaleAllSymbols(double factor)
{
	int size = getNumSymbols();
	for (int i = 0; i < size; ++i)
	{
		Symbol* symbol = getSymbol(i);
		symbol->scale(factor);
		emit(symbolChanged(i, symbol, symbol));
	}
	updateAllObjects();
	
	setSymbolsDirty();
}

void Map::determineSymbolsInUse(std::vector< bool >& out)
{
	out.assign(symbols.size(), false);
	for (std::size_t l = 0; l < parts.size(); ++l)
	{
		for (int o = 0; o < parts[l]->getNumObjects(); ++o)
		{
			const Symbol* symbol = parts[l]->getObject(o)->getSymbol();
			int index = findSymbolIndex(symbol);
			if (index >= 0)
				out[index] = true;
		}
	}
	
	determineSymbolUseClosure(out);
}

void Map::determineSymbolUseClosure(std::vector< bool >& symbol_bitfield)
{
	bool change;
	do
	{
		change = false;
		
		for (size_t i = 0, end = symbol_bitfield.size(); i < end; ++i)
		{
			if (symbol_bitfield[i])
				continue;
			
			// Check if this symbol is needed by any included symbol
			for (size_t k = 0; k < end; ++k)
			{
				if (i == k)
					continue;
				if (!symbol_bitfield[k])
					continue;
				if (symbols[k]->containsSymbol(symbols[i]))
				{
					symbol_bitfield[i] = true;
					change = true;
					break;
				}
			}
		}
		
	} while (change);
}

void Map::setTemplate(Template* temp, int pos)
{
	templates[pos] = temp;
	emit(templateChanged(pos, templates[pos]));
}

void Map::addTemplate(Template* temp, int pos, MapView* view)
{
	templates.insert(templates.begin() + pos, temp);
	if (templates.size() == 1)
	{
		// This is the first template - the help text in the map widget(s) should be updated
		updateAllMapWidgets();
	}
	
	if (view)
	{
		TemplateVisibility* vis = view->getTemplateVisibility(temp);
		vis->visible = true;
		vis->opacity = 1;
	}
	
	emit(templateAdded(pos, temp));
}

void Map::removeTemplate(int pos)
{
	TemplateVector::iterator it = templates.begin() + pos;
	Template* temp = *it;
	
	// Delete visibility information for the template from all views - get the views indirectly by iterating over all widgets
	for (MapWidget* widget : widgets)
		widget->getMapView()->deleteTemplateVisibility(temp);
	
	templates.erase(it);
	
	if (templates.empty())
	{
		// That was the last template - the help text in the map widget(s) should maybe be updated (if there are no objects)
		updateAllMapWidgets();
	}
	
	emit(templateDeleted(pos, temp));
}

void Map::deleteTemplate(int pos)
{
	Template* temp = getTemplate(pos);
	removeTemplate(pos);
	delete temp;
}

void Map::setTemplateAreaDirty(Template* temp, QRectF area, int pixel_border)
{
	bool front_cache = findTemplateIndex(temp) >= getFirstFrontTemplate();	// TODO: is there a better way to find out if that is a front or back template?
	
	for (MapWidget* widget : widgets)
	{
		const MapView* map_view = widget->getMapView();
		if (map_view->isTemplateVisible(temp))
			widget->markTemplateCacheDirty(map_view->calculateViewBoundingBox(area), pixel_border, front_cache);
	}
}

void Map::setTemplateAreaDirty(int i)
{
	if (i == -1)
		return;	// no assert here as convenience, so setTemplateAreaDirty(-1) can be called without effect for the map part
	Q_ASSERT(i >= 0 && i < (int)templates.size());
	
	templates[i]->setTemplateAreaDirty();
}

int Map::findTemplateIndex(const Template* temp) const
{
	int size = (int)templates.size();
	for (int i = 0; i < size; ++i)
	{
		if (templates[i] == temp)
			return i;
	}
	Q_ASSERT(false);
	return -1;
}

void Map::setTemplatesDirty()
{
	templates_dirty = true;
	setHasUnsavedChanges(true);
}

void Map::emitTemplateChanged(Template* temp)
{
	emit(templateChanged(findTemplateIndex(temp), temp));
}

void Map::clearClosedTemplates()
{
	if (closed_templates.size() == 0)
		return;
	
	for (Template* temp : closed_templates)
		delete temp;
	
	closed_templates.clear();
	setTemplatesDirty();
	emit closedTemplateAvailabilityChanged();
}

void Map::closeTemplate(int i)
{
	Template* temp = getTemplate(i);
	removeTemplate(i);
	
	if (temp->getTemplateState() == Template::Loaded)
		temp->unloadTemplateFile();
	
	closed_templates.push_back(temp);
	setTemplatesDirty();
	if (closed_templates.size() == 1)
		emit closedTemplateAvailabilityChanged();
}

bool Map::reloadClosedTemplate(int i, int target_pos, QWidget* dialog_parent, MapView* view, const QString& map_path)
{
	Template* temp = closed_templates[i];
	
	// Try to find and load the template again
	if (temp->getTemplateState() != Template::Loaded)
	{
		if (!temp->tryToFindAndReloadTemplateFile(map_path))
		{
			if (!temp->execSwitchTemplateFileDialog(dialog_parent))
				return false;
		}
	}
	
	// If successfully loaded, add to template list again
	if (temp->getTemplateState() == Template::Loaded)
	{
		closed_templates.erase(closed_templates.begin() + i);
		addTemplate(temp, target_pos, view);
		temp->setTemplateAreaDirty();
		setTemplatesDirty();
		if (closed_templates.size() == 0)
			emit closedTemplateAvailabilityChanged();
		return true;
	}
	return false;
}

void Map::push(UndoStep *step)
{
	undo_manager->push(step);
}


void Map::addPart(MapPart* part, std::size_t index)
{
	Q_ASSERT(index <= parts.size());
	
	parts.insert(parts.begin() + index, part);
	if (current_part_index >= index)
		setCurrentPartIndex(current_part_index + 1);
	
	emit mapPartAdded(index, part);
	
	setOtherDirty();
	updateAllMapWidgets();
}

void Map::removePart(std::size_t index)
{
	Q_ASSERT(index < parts.size());
	Q_ASSERT(parts.size() > 1);
	
	if (current_part_index == index)
		// First switch to another part when removing the current part
		setCurrentPartIndex((index == parts.size() - 1) ? (parts.size() - 2) : (index + 1));
	
	MapPart* part = parts[index];
	
	// FIXME: This loop should move to MapPart.
	while(part->getNumObjects())
		part->deleteObject(0, false);
	
	parts.erase(parts.begin() + index);
	if (current_part_index >= index)
		setCurrentPartIndex((index == parts.size()) ? (parts.size() - 1) : index);
	
	emit mapPartDeleted(index, part);
	
	delete part;
	
	setOtherDirty();
	updateAllMapWidgets();
}

int Map::findPartIndex(const MapPart* part) const
{
	std::size_t const size = parts.size();
	for (std::size_t i = 0; i < size; ++i)
	{
		if (parts[i] == part)
			return i;
	}
	Q_ASSERT(false);
	return -1;
}

void Map::setCurrentPartIndex(std::size_t index)
{
	Q_ASSERT(index < parts.size());
	
	MapPart* const old_part = parts[current_part_index];
	if (index != current_part_index)
	{
		current_part_index = index;
		emit currentMapPartIndexChanged(index);
	}
	
	MapPart* const new_part = parts[current_part_index];
	if (new_part != old_part)
	{
		clearObjectSelection(true);
		emit currentMapPartChanged(new_part);
	}
}

std::size_t Map::reassignObjectsToMapPart(std::set<Object*>::const_iterator begin, std::set<Object*>::const_iterator end, std::size_t source, std::size_t destination)
{
	Q_ASSERT(source < parts.size());
	Q_ASSERT(destination < parts.size());
	
	std::size_t count = 0;
	MapPart* const source_part = parts[source];
	MapPart* const target_part = parts[destination];
	for (std::set<Object*>::const_iterator it = begin; it != end; ++it)
	{
		Object* const object = *it;
		source_part->deleteObject(object, true);

		int index = target_part->getNumObjects();
		target_part->addObject(object, index);
		
		++count;
	}
	
	setOtherDirty();
	
	std::size_t const target_end   = target_part->getNumObjects();
	std::size_t const target_begin = target_end - count;
	
	if (current_part_index == source)
	{
		int const selection_size = getNumSelectedObjects();
		
		// When modifying the selection we must not use the original iterators
		// because they may be operating on the selection and then become invalid!
		for (std::size_t i = target_begin; i != target_end; ++i)
		{
			Object* const object = target_part->getObject(i);
			if (isObjectSelected(object))
				removeObjectFromSelection(object, false);
		}
		
		if (selection_size != getNumSelectedObjects())
			emit objectSelectionChanged();
	}	
		
	return target_begin;
}

std::size_t Map::reassignObjectsToMapPart(std::vector<int>::const_iterator begin, std::vector<int>::const_iterator end, std::size_t source, std::size_t destination)
{
	Q_ASSERT(source < parts.size());
	Q_ASSERT(destination < parts.size());
	
	bool selection_changed = false;
	
	std::size_t count = 0;
	MapPart* const source_part = parts[source];
	MapPart* const target_part = parts[destination];
	for (std::vector<int>::const_iterator it = begin; it != end; ++it)
	{
		Object* const object = source_part->getObject(*it);
		
		if (current_part_index == source && isObjectSelected(object))
		{
			removeObjectFromSelection(object, false);
			selection_changed = true;
		}
		
		source_part->deleteObject(object, true);
		
		int index = target_part->getNumObjects();
		target_part->addObject(object, index);
		
		++count;
	}
	
	setOtherDirty();
	
	if (selection_changed)
		emit objectSelectionChanged();
	
	return target_part->getNumObjects() - count;
}

std::size_t Map::mergeParts(std::size_t source, std::size_t destination)
{
	Q_ASSERT(source < parts.size());
	Q_ASSERT(destination < parts.size());
	
	std::size_t count = 0;
	MapPart* const source_part = parts[source];
	MapPart* const target_part = parts[destination];
	// Preserve order (but not efficient)
	for (std::size_t i = source_part->getNumObjects(); i > 0 ; --i)
	{
		Object* object = source_part->getObject(0);
		source_part->deleteObject(0, true);
		
		int index = target_part->getNumObjects();
		target_part->addObject(object, index);
		
		++count;
	}
	
	if (current_part_index == source)
		setCurrentPartIndex(destination);
	
	if (destination != source)
		removePart(source);
	
	return target_part->getNumObjects() - count;
}


int Map::getNumObjects()
{
	int num_objects = 0;
	for (const MapPart* part : parts)
		num_objects += part->getNumObjects();
	return num_objects;
}

int Map::addObject(Object* object, int part_index)
{
	MapPart* part = parts[(part_index < 0) ? current_part_index : part_index];
	int object_index = part->getNumObjects();
	part->addObject(object, object_index);
	
	return object_index;
}

void Map::deleteObject(Object* object, bool remove_only)
{
	for (MapPart* part : parts)
	{
		if (part->deleteObject(object, remove_only))
			return;
	}
	
	qCritical().nospace() << this << "::deleteObject(" << object << "," << remove_only << "): Object not found. This is a bug.";
	if (!remove_only)
		delete object;
}

void Map::setObjectsDirty()
{
	objects_dirty = true;
	setHasUnsavedChanges(true);
}

QRectF Map::calculateExtent(bool include_helper_symbols, bool include_templates, const MapView* view) const
{
	QRectF rect;
	
	// Objects
	for (const MapPart* part : parts)
		rectIncludeSafe(rect, part->calculateExtent(include_helper_symbols));
	
	// Templates
	if (include_templates)
	{
		for (const Template* temp : templates)
		{
			if (view && !view->isTemplateVisible(temp))
				continue;
			if (temp->getTemplateState() != Template::Loaded)
				continue;
			
			rectIncludeSafe(rect, temp->calculateTemplateBoundingBox());
		}
	}
	
	return rect;
}

void Map::setObjectAreaDirty(QRectF map_coords_rect)
{
	for (MapWidget* widget : widgets)
		widget->markObjectAreaDirty(map_coords_rect);
}

void Map::findObjectsAt(
        MapCoordF coord,
        float tolerance,
        bool treat_areas_as_paths,
        bool extended_selection,
        bool include_hidden_objects,
        bool include_protected_objects,
        SelectionInfoVector& out ) const
{
	getCurrentPart()->findObjectsAt(coord, tolerance, treat_areas_as_paths, extended_selection, include_hidden_objects, include_protected_objects, out);
}

void Map::findAllObjectsAt(
        MapCoordF coord,
        float tolerance,
        bool treat_areas_as_paths,
        bool extended_selection,
        bool include_hidden_objects,
        bool include_protected_objects,
        SelectionInfoVector& out ) const
{
	for (const MapPart* part : parts)
		part->findObjectsAt(coord, tolerance, treat_areas_as_paths, extended_selection, include_hidden_objects, include_protected_objects, out);
}

void Map::findObjectsAtBox(
        MapCoordF corner1,
        MapCoordF corner2,
        bool include_hidden_objects,
        bool include_protected_objects,
        std::vector< Object* >& out ) const
{
	getCurrentPart()->findObjectsAtBox(corner1, corner2, include_hidden_objects, include_protected_objects, out);
}

int Map::countObjectsInRect(QRectF map_coord_rect, bool include_hidden_objects)
{
	int count = 0;
	for (const MapPart* part : parts)
		count += part->countObjectsInRect(map_coord_rect, include_hidden_objects);
	return count;
}

void Map::scaleAllObjects(double factor, const MapCoord& scaling_center)
{
	applyOnAllObjects(ObjectOp::Scale(factor, scaling_center));
}

void Map::rotateAllObjects(double rotation, const MapCoord& center)
{
	applyOnAllObjects(ObjectOp::Rotate(rotation, center));
}

void Map::updateAllObjects()
{
	applyOnAllObjects(ObjectOp::ForceUpdate());
}

void Map::updateAllObjectsWithSymbol(const Symbol* symbol)
{
	applyOnMatchingObjects(ObjectOp::ForceUpdate(), ObjectOp::HasSymbol(symbol));
}

void Map::changeSymbolForAllObjects(const Symbol* old_symbol, const Symbol* new_symbol)
{
	applyOnMatchingObjects(ObjectOp::ChangeSymbol(new_symbol), ObjectOp::HasSymbol(old_symbol));
}

bool Map::deleteAllObjectsWithSymbol(const Symbol* symbol)
{
	bool exists = existsObject(ObjectOp::HasSymbol(symbol));
	if (exists)
	{
		// Remove objects from selection
		removeSymbolFromSelection(symbol, true);
	
		// Delete objects from map
		applyOnMatchingObjects(ObjectOp::Delete(), ObjectOp::HasSymbol(symbol));
	}
	return exists;
}

bool Map::existsObjectWithSymbol(const Symbol* symbol) const
{
	return existsObject(ObjectOp::HasSymbol(symbol));
}

void Map::setGeoreferencing(const Georeferencing& georeferencing)
{
	*this->georeferencing = georeferencing;
	setOtherDirty();
}

void Map::setGrid(const MapGrid &grid)
{
	if (grid != this->grid)
	{
		this->grid = grid;
		for (MapWidget* widget : widgets)
		{
			MapView* view = widget->getMapView();
			if (view && view->isGridVisible())
				view->updateAllMapWidgets();
		}
		setOtherDirty();
	}
}


bool Map::isAreaHatchingEnabled() const
{
	return renderable_options & Symbol::RenderAreasHatched;
}

void Map::setAreaHatchingEnabled(bool enabled)
{
	if (enabled)
		renderable_options |= Symbol::RenderAreasHatched;
	else
		renderable_options &= ~Symbol::RenderAreasHatched;
}


bool Map::isBaselineViewEnabled() const
{
	return renderable_options & Symbol::RenderBaselines;
}

void Map::setBaselineViewEnabled(bool enabled)
{
	if (enabled)
		renderable_options |= Symbol::RenderBaselines;
	else
		renderable_options &= ~Symbol::RenderBaselines;
}


const MapPrinterConfig& Map::printerConfig()
{
	if (printer_config.isNull())
		printer_config.reset(new MapPrinterConfig(*this));
	
	return *printer_config;
}

MapPrinterConfig Map::printerConfig() const
{
	MapPrinterConfig ret = printer_config.isNull() ? MapPrinterConfig{ *this } : *printer_config;
	return ret;
}

void Map::setPrinterConfig(const MapPrinterConfig& config)
{
	if (printer_config.isNull())
	{
		printer_config.reset(new MapPrinterConfig(config));
		setOtherDirty();
	}
	else if (*printer_config != config)
	{
		*printer_config = config;
		setOtherDirty();
	}
}

void Map::setImageTemplateDefaults(bool use_meters_per_pixel, double meters_per_pixel, double dpi, double scale)
{
	image_template_use_meters_per_pixel = use_meters_per_pixel;
	image_template_meters_per_pixel = meters_per_pixel;
	image_template_dpi = dpi;
	image_template_scale = scale;
}

void Map::getImageTemplateDefaults(bool& use_meters_per_pixel, double& meters_per_pixel, double& dpi, double& scale)
{
	use_meters_per_pixel = image_template_use_meters_per_pixel;
	meters_per_pixel = image_template_meters_per_pixel;
	dpi = image_template_dpi;
	scale = image_template_scale;
}

void Map::setHasUnsavedChanges(bool has_unsaved_changes)
{
	if (!has_unsaved_changes)
	{
		colors_dirty = false;
		symbols_dirty = false;
		templates_dirty = false;
		objects_dirty = false;
		other_dirty = false;
		if (unsaved_changes)
		{
			unsaved_changes = false;
			emit hasUnsavedChanges(unsaved_changes);
		}
	}
	else if (!unsaved_changes)
	{
		unsaved_changes = true;
		emit hasUnsavedChanges(unsaved_changes);
	}
}

void Map::setOtherDirty()
{
	other_dirty = true;
	setHasUnsavedChanges(true);
}

// slot
void Map::undoCleanChanged(bool is_clean)
{
	if (is_clean && unsaved_changes && !(colors_dirty || symbols_dirty || templates_dirty || other_dirty))
	{
		setHasUnsavedChanges(false);
	}
	else if (!is_clean && !unsaved_changes)
	{
		setHasUnsavedChanges(true);
	}
}