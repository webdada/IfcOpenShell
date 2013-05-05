﻿/********************************************************************************
 *                                                                              *
 * This file is part of IfcOpenShell.                                           *
 *                                                                              *
 * IfcOpenShell is free software: you can redistribute it and/or modify         *
 * it under the terms of the Lesser GNU General Public License as published by  *
 * the Free Software Foundation, either version 3.0 of the License, or          *
 * (at your option) any later version.                                          *
 *                                                                              *
 * IfcOpenShell is distributed in the hope that it will be useful,              *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
 * Lesser GNU General Public License for more details.                          *
 *                                                                              *
 * You should have received a copy of the Lesser GNU General Public License     *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.         *
 *                                                                              *
 ********************************************************************************/

#include <map>

#include <gp_Mat.hxx>
#include <gp_Mat2d.hxx>
#include <gp_GTrsf.hxx>
#include <gp_GTrsf2d.hxx>
#include <gp_Trsf.hxx>
#include <gp_Trsf2d.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepMesh.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TShort_Array1OfShortReal.hxx>
#include <Poly_Array1OfTriangle.hxx>
#include <StdFail_NotDone.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepBuilderAPI_GTransform.hxx>

#include "../ifcparse/IfcException.h"
#include "../ifcgeom/IfcGeomObjects.h"
#include "../ifcgeom/IfcGeom.h"

// Welds vertices that belong to different faces
bool weld_vertices = true;

bool convert_back_units = false;
bool use_faster_booleans = false;
bool disable_subtractions = false;
bool disable_triangulation = false;

int IfcGeomObjects::IfcRepresentationTriangulation::addvert(int material_index, const gp_XYZ& p) {
	const float X = convert_back_units ? (float) (p.X() / IfcGeom::GetValue(IfcGeom::GV_LENGTH_UNIT)) : (float)p.X();
	const float Y = convert_back_units ? (float) (p.Y() / IfcGeom::GetValue(IfcGeom::GV_LENGTH_UNIT)) : (float)p.Y();
	const float Z = convert_back_units ? (float) (p.Z() / IfcGeom::GetValue(IfcGeom::GV_LENGTH_UNIT)) : (float)p.Z();
	int i = (int) verts.size() / 3;
	if ( weld_vertices ) {
		const VertKey key = std::make_pair(material_index, std::make_pair(X, std::make_pair(Y, Z)));
		VertKeyMap::const_iterator it = welds.find(key);
		if ( it != welds.end() ) return it->second;
		i = (int) welds.size();
		welds[key] = i;
	}
	verts.push_back(X);
	verts.push_back(Y);
	verts.push_back(Z);
	return i;
}

bool use_world_coords = false;
bool use_brep_data = false;

static IfcParse::IfcFile* ifc_file = 0;

IfcGeomObjects::IfcRepresentationBrepData::IfcRepresentationBrepData(const IfcRepresentationShapeModel& shapes)
	: id(shapes.getId()) 
{
	try {
		TopoDS_Compound compound;
		BRep_Builder builder;
		builder.MakeCompound(compound);
		for ( IfcGeom::IfcRepresentationShapeItems::const_iterator it = shapes.begin(); it != shapes.end(); ++ it ) {
			const TopoDS_Shape& s = it->Shape();
			const gp_GTrsf& trsf = it->Placement();
			bool trsf_valid = false;
			gp_Trsf _trsf;
			try {
				_trsf = trsf.Trsf();
				trsf_valid = true;
			} catch (...) {}
			const TopoDS_Shape moved_shape = trsf_valid ? s.Moved(_trsf) :
				BRepBuilderAPI_GTransform(s,trsf,true).Shape();
			builder.Add(compound,moved_shape);
		}
		std::stringstream sstream;
		BRepTools::Write(compound,sstream);
		brep_data = sstream.str();
	} catch(...) {
		Logger::Message(Logger::LOG_ERROR,"Failed to serialize shape:",ifc_file->EntityById(id)->entity);
	}
}

IfcGeomObjects::IfcRepresentationTriangulation::IfcRepresentationTriangulation(const IfcRepresentationShapeModel& shapes)
	: id(shapes.getId()) 
{
	for ( IfcGeom::IfcRepresentationShapeItems::const_iterator it = shapes.begin(); it != shapes.end(); ++ it ) {

		int surface_style_id = -1;
		if (it->hasStyle()) {
			std::vector<const IfcGeom::SurfaceStyle*>::const_iterator jt = std::find(surface_styles.begin(), surface_styles.end(), &it->Style());
			if (jt == surface_styles.end()) {
				surface_style_id = surface_styles.size();
				surface_styles.push_back(&it->Style());
			} else {
				surface_style_id = jt - surface_styles.begin();
			}
		}

		const TopoDS_Shape& s = it->Shape();
		const gp_GTrsf& trsf = it->Placement();

		// Triangulate the shape
		try {
			// BRepTools::Clean(s);
			BRepMesh::Mesh(s, IfcGeom::GetValue(IfcGeom::GV_DEFLECTION_TOLERANCE));
		} catch(...) {
			Logger::Message(Logger::LOG_ERROR,"Failed to triangulate shape:",ifc_file->EntityById(id)->entity);
			continue;
		}
		TopExp_Explorer exp;

		// Iterates over the faces of the shape
		for ( exp.Init(s,TopAbs_FACE); exp.More(); exp.Next() ) {
			TopoDS_Face face = TopoDS::Face(exp.Current());
			TopLoc_Location loc;
			Handle_Poly_Triangulation tri = BRep_Tool::Triangulation(face,loc);

			if ( ! tri.IsNull() ) {

				// A 3x3 matrix to rotate the vertex normals
				const gp_Mat rotation_matrix = trsf.VectorialPart();
			
				// Keep track of the number of times an edge is used
				// Manifold edges (i.e. edges used twice) are deemed invisible
				std::map<std::pair<int,int>,int> edgecount;
				std::vector<std::pair<int,int> > edges_temp;

				const TColgp_Array1OfPnt& nodes = tri->Nodes();
				const TColgp_Array1OfPnt2d& uvs = tri->UVNodes();
				std::vector<gp_XYZ> coords;
				BRepGProp_Face prop(face);
				std::map<int,int> dict;

				// Vertex normals are only calculated if vertices are not welded
				const bool calculate_normals = ! weld_vertices;

				for( int i = 1; i <= nodes.Length(); ++ i ) {
					coords.push_back(nodes(i).Transformed(loc).XYZ());
					trsf.Transforms(*coords.rbegin());
					dict[i] = addvert(surface_style_id, *coords.rbegin());
					
					if ( calculate_normals ) {
						const gp_Pnt2d& uv = uvs(i);
						gp_Pnt p;
						gp_Vec normal_direction;
						prop.Normal(uv.X(),uv.Y(),p,normal_direction);						
						gp_Dir normal = gp_Dir(normal_direction.XYZ() * rotation_matrix);
						normals.push_back((float)normal.X());
						normals.push_back((float)normal.Y());
						normals.push_back((float)normal.Z());
					}
				}

				const Poly_Array1OfTriangle& triangles = tri->Triangles();			
				for( int i = 1; i <= triangles.Length(); ++ i ) {
					int n1,n2,n3;
					if ( face.Orientation() == TopAbs_REVERSED )
						triangles(i).Get(n3,n2,n1);
					else triangles(i).Get(n1,n2,n3);

					/* An alternative would be to calculate normals based
					 * on the coordinates of the mesh vertices */
					/*
					const gp_XYZ pt1 = coords[n1-1];
					const gp_XYZ pt2 = coords[n2-1];
					const gp_XYZ pt3 = coords[n3-1];
					const gp_XYZ v1 = pt2-pt1;
					const gp_XYZ v2 = pt3-pt2;
					gp_Dir normal = gp_Dir(v1^v2);
					normals.push_back((float)normal.X());
					normals.push_back((float)normal.Y());
					normals.push_back((float)normal.Z());
					*/

					faces.push_back(dict[n1]);
					faces.push_back(dict[n2]);
					faces.push_back(dict[n3]);

					materials.push_back(surface_style_id);

					addedge(n1,n2,edgecount,edges_temp);
					addedge(n2,n3,edgecount,edges_temp);
					addedge(n3,n1,edgecount,edges_temp);
				}
				for ( std::vector<std::pair<int,int> >::const_iterator it = edges_temp.begin(); it != edges_temp.end(); ++it ) {
					edges.push_back(edgecount[*it]==1);
				}
			}
		}
	}
}

IfcGeomObjects::IfcObject::IfcObject(
	int id, 
	int parent_id, 
	const std::string& name, 
	const std::string& type, 
	const std::string& guid, 
	const gp_Trsf& trsf)
		: id(id)
		, parent_id(parent_id)
		, name(name)
		, type(type)
		, guid(guid)
		, trsf(trsf)
{
	// Convert the gp_Trsf into a 4x3 Matrix
	for( int i = 1; i < 5; ++ i )
		for ( int j = 1; j < 4; ++ j )
			matrix.push_back((float)trsf.Value(j,i));
}

IfcGeomObjects::IfcGeomShapeModelObject::IfcGeomShapeModelObject(
	int id, 
	int parent_id, 
	const std::string& name, 
	const std::string& type, 
	const std::string& guid,
	const gp_Trsf& trsf, 
	IfcRepresentationShapeModel* shapes) 
		: IfcObject(id,parent_id,name,type,guid,trsf)
		, mesh(shapes)
{}

IfcGeomObjects::IfcGeomBrepDataObject::IfcGeomBrepDataObject(
	const IfcGeomShapeModelObject& shape_model)
		: IfcObject(shape_model)
		, mesh(new IfcRepresentationBrepData(*shape_model.mesh))
{}

IfcGeomObjects::IfcGeomObject::IfcGeomObject(
	const IfcGeomShapeModelObject& shape_model)
		: IfcObject(shape_model)
		, mesh(new IfcRepresentationTriangulation(*shape_model.mesh))
{}

// A container and iterator for IfcShapeRepresentations
Ifc2x3::IfcShapeRepresentation::list shapereps;
Ifc2x3::IfcShapeRepresentation::it shaperep_iterator;

// The object is fetched beforehand to be positive an entity actually exists
IfcGeomObjects::IfcGeomObject* current_geom_obj = 0;
IfcGeomObjects::IfcGeomShapeModelObject* current_shape_model_obj = 0;
IfcGeomObjects::IfcGeomBrepDataObject* current_brep_data_obj = 0;

// A container and iterator for IfcBuildingElements for the current IfcShapeRepresentation referenced by *shaperep_iterator
Ifc2x3::IfcProduct::list entities;
Ifc2x3::IfcProduct::it ifcproduct_iterator;

int done;
int total;

// Move the the next IfcShapeRepresentation
void _nextShape() {
	entities.reset();
	++ shaperep_iterator;
	++ done;
}

int _getParentId(const Ifc2x3::IfcProduct::ptr ifc_product) {
	int parent_id = -1;
	// In case of an opening element, parent to the RelatingBuildingElement
	if ( ifc_product->is(Ifc2x3::Type::IfcOpeningElement ) ) {
		Ifc2x3::IfcOpeningElement::ptr opening = reinterpret_pointer_cast<Ifc2x3::IfcProduct,Ifc2x3::IfcOpeningElement>(ifc_product);
		Ifc2x3::IfcRelVoidsElement::list voids = opening->VoidsElements();
		if ( voids->Size() ) {
			Ifc2x3::IfcRelVoidsElement::ptr ifc_void = *voids->begin();
			parent_id = ifc_void->RelatingBuildingElement()->entity->id();
		}
	} else if ( ifc_product->is(Ifc2x3::Type::IfcElement ) ) {
		Ifc2x3::IfcElement::ptr element = reinterpret_pointer_cast<Ifc2x3::IfcProduct,Ifc2x3::IfcElement>(ifc_product);
		Ifc2x3::IfcRelFillsElement::list fills = element->FillsVoids();
		// Incase of a RelatedBuildingElement parent to the opening element
		if ( fills->Size() ) {
			for ( Ifc2x3::IfcRelFillsElement::it it = fills->begin(); it != fills->end(); ++ it ) {
				Ifc2x3::IfcRelFillsElement::ptr fill = *it;
				Ifc2x3::IfcObjectDefinition* ifc_objectdef = fill->RelatingOpeningElement();
				if ( ifc_product == ifc_objectdef ) continue;
				parent_id = ifc_objectdef->entity->id();
			}
		} 
		// Else simply parent to the containing structure
		if ( parent_id == -1 ) {
			Ifc2x3::IfcRelContainedInSpatialStructure::list parents = element->ContainedInStructure();
			if ( parents->Size() ) {
				Ifc2x3::IfcRelContainedInSpatialStructure::ptr parent = *parents->begin();
				parent_id = parent->RelatingStructure()->entity->id();
			}
		}
	}
	// Parent decompositions to the RelatingObject
	if ( parent_id == -1 ) {
		IfcEntities parents = ifc_product->entity->getInverse(Ifc2x3::Type::IfcRelAggregates);
		parents->push(ifc_product->entity->getInverse(Ifc2x3::Type::IfcRelNests));
		for ( IfcEntityList::it it = parents->begin(); it != parents->end(); ++ it ) {
			Ifc2x3::IfcRelDecomposes::ptr decompose = reinterpret_pointer_cast<IfcBaseClass,Ifc2x3::IfcRelDecomposes>(*it);
			Ifc2x3::IfcObjectDefinition* ifc_objectdef = decompose->RelatingObject();
			if ( ifc_product == ifc_objectdef ) continue;
			parent_id = ifc_objectdef->entity->id();
		}
	}
	return parent_id;
}

IfcGeomObjects::IfcGeomShapeModelObject* create_shape_model_for_next_entity() {
	while ( true ) {
		Ifc2x3::IfcShapeRepresentation::ptr shaperep;

		// Have we reached the end of our list of representations?
		if ( shaperep_iterator == shapereps->end() ) {
			shapereps.reset();
			return 0;
		}
		shaperep = *shaperep_iterator;

		// Has the list of IfcProducts for this representation been initialized?
		if ( ! entities ) {
			if ( shaperep->hasRepresentationIdentifier() ) {
				const std::string representation_identifier = shaperep->RepresentationIdentifier();
				if ( shaperep->hasRepresentationType() && representation_identifier == "IAI" && shaperep->RepresentationType() != "BoundingBox" ) {
					// Allow for Ifc 2x compatibility
				} else if ( representation_identifier != "Body" &&
					representation_identifier != "Facetation" ) {
						_nextShape();
						continue;
				}
			}
			Ifc2x3::IfcProductRepresentation::list prodreps = shaperep->OfProductRepresentation();
			entities = Ifc2x3::IfcProduct::list( new IfcTemplatedEntityList<Ifc2x3::IfcProduct>() );
			for ( Ifc2x3::IfcProductRepresentation::it it = prodreps->begin(); it != prodreps->end(); ++it ) {
				if ( (*it)->is(Ifc2x3::Type::IfcProductDefinitionShape) ) {
					Ifc2x3::IfcProductDefinitionShape::ptr pds = reinterpret_pointer_cast<Ifc2x3::IfcProductRepresentation,Ifc2x3::IfcProductDefinitionShape>(*it);
					entities->push(pds->ShapeOfProduct());
				} else {
					// http://buildingsmart-tech.org/ifc/IFC2x3/TC1/html/ifcrepresentationresource/lexical/ifcproductrepresentation.htm
					// IFC2x Edition 3 NOTE  Users should not instantiate the entity IfcProductRepresentation from IFC2x Edition 3 onwards. 
					// It will be changed into an ABSTRACT supertype in future releases of IFC.

					// IfcProductRepresentation also lacks the INVERSE relation to IfcProduct
					// Let's find the IfcProducts that reference the IfcProductRepresentation anyway
					IfcEntities products = (*it)->entity->getInverse(Ifc2x3::Type::IfcProduct);
					for ( IfcEntityList::it it = products->begin(); it != products->end(); ++ it ) {
						entities->push(reinterpret_pointer_cast<IfcBaseClass,Ifc2x3::IfcProduct>(*it));
					}
				}
			}
			// Does this representation have any IfcProducts?
			if ( ! entities->Size() ) {
				_nextShape();
				continue;
			}
			ifcproduct_iterator = entities->begin();
		}
		// Have we reached the end of our list of IfcProducts?
		if ( ifcproduct_iterator == entities->end() ) {
			_nextShape();
			continue;
		}
		
		IfcGeomObjects::IfcRepresentationShapeModel* shape;
		IfcGeom::IfcRepresentationShapeItems shapes;

		if ( !IfcGeom::convert_shapes(shaperep,shapes) ) {
			_nextShape();
			continue;
		}

		Ifc2x3::IfcProduct::ptr ifc_product = *ifcproduct_iterator;

		int parent_id = -1;
		try {
			parent_id = _getParentId(ifc_product);
		} catch (...) {}
		
		const std::string name = ifc_product->hasName() ? ifc_product->Name() : "";
		const std::string guid = ifc_product->GlobalId();
		
		gp_Trsf trsf;
		try {
			IfcGeom::convert(ifc_product->ObjectPlacement(),trsf);
		} catch (...) {}

		// Does the IfcElement have any IfcOpenings?
		// Note that openings for IfcOpeningElements are not processed
		Ifc2x3::IfcRelVoidsElement::list openings = Ifc2x3::IfcRelVoidsElement::list();
		if ( ifc_product->is(Ifc2x3::Type::IfcElement) && !ifc_product->is(Ifc2x3::Type::IfcOpeningElement) ) {
			Ifc2x3::IfcElement::ptr element = reinterpret_pointer_cast<Ifc2x3::IfcProduct,Ifc2x3::IfcElement>(ifc_product);
			openings = element->HasOpenings();
		}
		// Is the IfcElement a decomposition of an IfcElement with any IfcOpeningElements?
		if ( ifc_product->is(Ifc2x3::Type::IfcBuildingElementPart ) ) {
			Ifc2x3::IfcBuildingElementPart::ptr part = reinterpret_pointer_cast<Ifc2x3::IfcProduct,Ifc2x3::IfcBuildingElementPart>(ifc_product);
			Ifc2x3::IfcRelDecomposes::list decomposes = part->Decomposes();
			for ( Ifc2x3::IfcRelDecomposes::it it = decomposes->begin(); it != decomposes->end(); ++ it ) {
				Ifc2x3::IfcObjectDefinition::ptr obdef = (*it)->RelatingObject();
				if ( obdef->is(Ifc2x3::Type::IfcElement) ) {
					Ifc2x3::IfcElement::ptr element = reinterpret_pointer_cast<Ifc2x3::IfcObjectDefinition,Ifc2x3::IfcElement>(obdef);
					openings->push(element->HasOpenings());
				}
			}
		}

		if ( !disable_subtractions && openings && openings->Size() ) {
			IfcGeom::IfcRepresentationShapeItems opened_shapes;
			try {
				if ( use_faster_booleans ) {
					bool succes = IfcGeom::convert_openings_fast(ifc_product,openings,shapes,trsf,opened_shapes);
					if ( ! succes ) {
						opened_shapes.clear();
						IfcGeom::convert_openings(ifc_product,openings,shapes,trsf,opened_shapes);
					}
				} else {
					IfcGeom::convert_openings(ifc_product,openings,shapes,trsf,opened_shapes);
				}
			} catch(...) { 
				Logger::Message(Logger::LOG_ERROR,"Error processing openings for:",ifc_product->entity); 
			}
			if ( use_world_coords ) {
				for ( IfcGeom::IfcRepresentationShapeItems::iterator it = opened_shapes.begin(); it != opened_shapes.end(); ++ it ) {
					it->move(trsf);
				}
				trsf = gp_Trsf();
			}
			shape = new IfcGeomObjects::IfcRepresentationShapeModel(shaperep->entity->id(),opened_shapes,true);
		} else if ( use_world_coords ) {
			for ( IfcGeom::IfcRepresentationShapeItems::iterator it = shapes.begin(); it != shapes.end(); ++ it ) {
				it->move(trsf);
			}
			trsf = gp_Trsf();
			shape = new IfcGeomObjects::IfcRepresentationShapeModel(shaperep->entity->id(),shapes);
		} else {
			shape = new IfcGeomObjects::IfcRepresentationShapeModel(shaperep->entity->id(),shapes);
		}

		return new IfcGeomObjects::IfcGeomShapeModelObject(ifc_product->entity->id(), parent_id, name, 
			Ifc2x3::Type::ToString(ifc_product->type()), guid, trsf, shape);
	}	
}

bool try_and_create_representations_for_current_entity() {
	current_shape_model_obj = create_shape_model_for_next_entity();
	if (current_shape_model_obj == 0) {
		return false;
	}
	
	if (use_brep_data) {
		current_brep_data_obj = new IfcGeomObjects::IfcGeomBrepDataObject(*current_shape_model_obj);
		if (current_brep_data_obj == 0) {
			return false;
		}
	}

	if (!disable_triangulation) {
		current_geom_obj = new IfcGeomObjects::IfcGeomObject(*current_shape_model_obj);
		if (current_geom_obj == 0) {
			return false;
		}
	}

	return true;
}

bool IfcGeomObjects::Next() {
	// Free all possible representations of the current geometrical entity
	delete current_geom_obj;
	delete current_brep_data_obj;
	delete current_shape_model_obj;
	current_geom_obj = 0;
	current_brep_data_obj = 0;
	current_shape_model_obj = 0;

	// Increment the iterator over the list of products using the current
	// shape representation
	if (entities) {
		++ifcproduct_iterator;
	}

	return try_and_create_representations_for_current_entity();
}

std::vector<IfcGeomObjects::IfcObject*> returned_objects;
bool IfcGeomObjects::CleanUp() {
	// TODO: Correctly implement destructor for IfcFile
	delete ifc_file;
	IfcGeom::Cache::Purge();
	std::vector<IfcGeomObjects::IfcObject*>::const_iterator it;
	for (it = returned_objects.begin(); it != returned_objects.end(); ++ it ) {
		delete *it;
	}
	returned_objects.clear();
	return true;
}
const IfcGeomObjects::IfcObject* IfcGeomObjects::GetObject(int id) {
	IfcObject* ifc_object = 0;
	try {
		const IfcParse::IfcEntity& ifc_entity = ifc_file->EntityById(id);
		if ( ifc_entity->is(Ifc2x3::Type::IfcProduct) ) {
			Ifc2x3::IfcProduct::ptr ifc_product = reinterpret_pointer_cast<IfcUtil::IfcBaseClass,Ifc2x3::IfcProduct>(ifc_entity);
			int parent_id = -1;
			try {
				parent_id = _getParentId(ifc_product);
			} catch (...) {}
			const std::string name = ifc_product->hasName() ? ifc_product->Name() : "";
			gp_Trsf trsf;
			try {
				IfcGeom::convert(ifc_product->ObjectPlacement(),trsf);
			} catch (...) {}
			ifc_object = new IfcObject(ifc_product->entity->id(),parent_id,name,
				Ifc2x3::Type::ToString(ifc_product->type()),ifc_product->GlobalId(),trsf);
		}
	} catch(...) {}
	if ( !ifc_object ) ifc_object = new IfcObject(-1,-1,"","","",gp_Trsf());
	returned_objects.push_back(ifc_object);
	return ifc_object;
}
const IfcGeomObjects::IfcGeomObject* IfcGeomObjects::Get() {
	return current_geom_obj;
}
const IfcGeomObjects::IfcGeomShapeModelObject* IfcGeomObjects::GetShapeModel() {
	return current_shape_model_obj;
}
const IfcGeomObjects::IfcGeomBrepDataObject* IfcGeomObjects::GetBrepData() {
	return current_brep_data_obj;
}
double UnitPrefixToValue( Ifc2x3::IfcSIPrefix::IfcSIPrefix v ) {
	if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_EXA   ) return (double) 1e18;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_PETA  ) return (double) 1e15;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_TERA  ) return (double) 1e12;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_GIGA  ) return (double) 1e9;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_MEGA  ) return (double) 1e6;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_KILO  ) return (double) 1e3;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_HECTO ) return (double) 1e2;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_DECA  ) return (double) 1;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_DECI  ) return (double) 1e-1;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_CENTI ) return (double) 1e-2;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_MILLI ) return (double) 1e-3;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_MICRO ) return (double) 1e-6;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_NANO  ) return (double) 1e-9;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_PICO  ) return (double) 1e-12;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_FEMTO ) return (double) 1e-15;
	else if ( v == Ifc2x3::IfcSIPrefix::IfcSIPrefix_ATTO  ) return (double) 1e-18;
	else return 1.0f;
}

void IfcGeomObjects::InitUnits() {
	// Set default units, set length to meters, angles to undefined
	IfcGeom::SetValue(IfcGeom::GV_LENGTH_UNIT,1.0);
	IfcGeom::SetValue(IfcGeom::GV_PLANEANGLE_UNIT,-1.0);
	
	Ifc2x3::IfcUnitAssignment::list unit_assignments = ifc_file->EntitiesByType<Ifc2x3::IfcUnitAssignment>();
	IfcUtil::IfcAbstractSelect::list units = IfcUtil::IfcAbstractSelect::list();
	if ( unit_assignments->Size() ) {
		Ifc2x3::IfcUnitAssignment::ptr unit_assignment = *unit_assignments->begin();
		units = unit_assignment->Units();
	}
	if ( ! units ) {
		// No units eh... Since tolerances and deflection are specified internally in meters
		// we will try to find another indication of the model size.
		Ifc2x3::IfcExtrudedAreaSolid::list extrusions = ifc_file->EntitiesByType<Ifc2x3::IfcExtrudedAreaSolid>();
		if ( ! extrusions->Size() ) return;
		double max_height = -1.0f;
		for ( Ifc2x3::IfcExtrudedAreaSolid::it it = extrusions->begin(); it != extrusions->end(); ++ it ) {
			const double depth = (*it)->Depth();
			if ( depth > max_height ) max_height = depth;
		}
		if ( max_height > 100.0f ) IfcGeom::SetValue(IfcGeom::GV_LENGTH_UNIT,0.001);
		return;
	}
	try {
		for ( IfcUtil::IfcAbstractSelect::it it = units->begin(); it != units->end(); ++ it ) {
			const IfcUtil::IfcAbstractSelect::ptr base = *it;
			Ifc2x3::IfcSIUnit::ptr unit = Ifc2x3::IfcSIUnit::ptr();
			double value = 1.0f;
			if ( base->is(Ifc2x3::Type::IfcConversionBasedUnit) ) {
				const Ifc2x3::IfcConversionBasedUnit::ptr u = reinterpret_pointer_cast<IfcUtil::IfcAbstractSelect,Ifc2x3::IfcConversionBasedUnit>(base);
				const Ifc2x3::IfcMeasureWithUnit::ptr u2 = u->ConversionFactor();
				Ifc2x3::IfcUnit u3 = u2->UnitComponent();
				if ( u3->is(Ifc2x3::Type::IfcSIUnit) ) {
					unit = (Ifc2x3::IfcSIUnit*) u3;
				}
				Ifc2x3::IfcValue v = u2->ValueComponent();
				IfcUtil::IfcArgumentSelect* v2 = (IfcUtil::IfcArgumentSelect*) v;
				const double f = *v2->wrappedValue();
				value *= f;
			} else if ( base->is(Ifc2x3::Type::IfcSIUnit) ) {
				unit = reinterpret_pointer_cast<IfcUtil::IfcAbstractSelect,Ifc2x3::IfcSIUnit>(base);
			}
			if ( unit ) {
				if ( unit->hasPrefix() ) {
					value *= UnitPrefixToValue(unit->Prefix());
				}
				Ifc2x3::IfcUnitEnum::IfcUnitEnum type = unit->UnitType();
				if ( type == Ifc2x3::IfcUnitEnum::IfcUnit_LENGTHUNIT ) {
					IfcGeom::SetValue(IfcGeom::GV_LENGTH_UNIT,value);
				} else if ( type == Ifc2x3::IfcUnitEnum::IfcUnit_PLANEANGLEUNIT ) {
					IfcGeom::SetValue(IfcGeom::GV_PLANEANGLE_UNIT,value);
				}
			}
		}
	} catch ( IfcException ex ) {
		Logger::Message(Logger::LOG_ERROR,ex.what());
	}
}

bool IfcGeomObjects::Init(const std::string fn) {
	return IfcGeomObjects::Init(fn, 0, 0);
}
bool _Init() {
	IfcGeomObjects::InitUnits();

	shapereps = ifc_file->EntitiesByType<Ifc2x3::IfcShapeRepresentation>();
	if ( ! shapereps ) return false;
	
	shaperep_iterator = shapereps->begin();
	entities.reset();

	if (!try_and_create_representations_for_current_entity()) {
		return false;
	}

	done = 0;
	total = shapereps->Size();
	return true;
}
bool IfcGeomObjects::Init(const std::string fn, std::ostream* log1, std::ostream* log2) {
	Logger::SetOutput(log1,log2);
	ifc_file = new IfcParse::IfcFile();
	if ( !ifc_file->Init(fn) ) return false;
	return _Init();	
}
bool IfcGeomObjects::Init(std::istream& f, int len, std::ostream* log1, std::ostream* log2) {
	Logger::SetOutput(log1,log2);
	ifc_file = new IfcParse::IfcFile();
	if ( !ifc_file->Init(f, len) ) return false;
	return _Init();
}
bool IfcGeomObjects::Init(void* data, int len) {
	Logger::SetOutput(0,0);
	ifc_file = new IfcParse::IfcFile();
	if ( !ifc_file->Init(data, len) ) return false;
	return _Init();
}
void IfcGeomObjects::Settings(int setting, bool value) {
	switch ( setting ) {
	case USE_WORLD_COORDS:
		use_world_coords = value;
		break;
	case WELD_VERTICES:
		weld_vertices = value;
		break;
	case CONVERT_BACK_UNITS:
		convert_back_units = value;
		break;
	case USE_BREP_DATA:
		use_brep_data = value;
		break;
	case FASTER_BOOLEANS:
		use_faster_booleans = value;
		break;
	case SEW_SHELLS:
		IfcGeom::SetValue(IfcGeom::GV_MAX_FACES_TO_SEW,value ? 1000 : -1);
		break;
	case FORCE_CCW_FACE_ORIENTATION:
		IfcGeom::SetValue(IfcGeom::GV_FORCE_CCW_FACE_ORIENTATION,value ? 1 : -1);
		break;
    case DISABLE_OPENING_SUBTRACTIONS:
        disable_subtractions = value;
        break;
	case DISABLE_TRIANGULATION:
        disable_triangulation = value;
        break;
	}
}
int IfcGeomObjects::Progress() {
	return 100 * done / total;
}
std::string IfcGeomObjects::GetLog() {
	return Logger::GetLog();
}

IfcParse::IfcFile* IfcGeomObjects::GetFile() {
  return ifc_file;
}
