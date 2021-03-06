/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2002 University of Southern California
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is University of Southern
 * California.
 *
 * Contributor(s):
 *	Carl D. Worth <cworth@cworth.org>
 */

#include "cairoint.h"
#include "cairo-path-fixed-private.h"
#include "cairo-gral-private.h"

typedef struct cairo_stroker {
    cairo_stroke_style_t	*style;

    cairo_matrix_t *ctm;
    cairo_matrix_t *ctm_inverse;
    double tolerance;
    double ctm_determinant;
    cairo_bool_t ctm_det_positive;

    cairo_gral_stroke_path_mesh_t *mesh;

    cairo_pen_t	  pen;

    cairo_point_t current_point;
    cairo_point_t first_point;

    cairo_bool_t has_initial_sub_path;

    cairo_bool_t has_current_face;
    cairo_stroke_face_t current_face;

    cairo_bool_t has_first_face;
    cairo_stroke_face_t first_face;

    cairo_bool_t dashed;
    unsigned int dash_index;
    cairo_bool_t dash_on;
    cairo_bool_t dash_starts_on;
    double dash_remain;
} cairo_stroker_t;

static void
_cairo_stroker_start_dash (cairo_stroker_t *stroker)
{
    double offset;
    cairo_bool_t on = TRUE;
    unsigned int i = 0;

    offset = stroker->style->dash_offset;

    /* We stop searching for a starting point as soon as the
       offset reaches zero.  Otherwise when an initial dash
       segment shrinks to zero it will be skipped over. */
    while (offset > 0.0 && offset >= stroker->style->dash[i]) {
	offset -= stroker->style->dash[i];
	on = !on;
	if (++i == stroker->style->num_dashes)
	    i = 0;
    }
    stroker->dashed = TRUE;
    stroker->dash_index = i;
    stroker->dash_on = stroker->dash_starts_on = on;
    stroker->dash_remain = stroker->style->dash[i] - offset;
}

static void
_cairo_stroker_step_dash (cairo_stroker_t *stroker, double step)
{
    stroker->dash_remain -= step;
    if (stroker->dash_remain <= 0) {
	stroker->dash_index++;
	if (stroker->dash_index == stroker->style->num_dashes)
	    stroker->dash_index = 0;
	stroker->dash_on = !stroker->dash_on;
	stroker->dash_remain = stroker->style->dash[stroker->dash_index];
    }
}

static cairo_status_t
_cairo_stroker_init (cairo_stroker_t		*stroker,
		     cairo_stroke_style_t	*stroke_style,
		     cairo_matrix_t		*ctm,
		     cairo_matrix_t		*ctm_inverse,
		     double			 tolerance,
		     cairo_gral_stroke_path_mesh_t *mesh)
{
    cairo_status_t status;
    stroker->style = stroke_style;
    stroker->ctm = ctm;
    stroker->ctm_inverse = ctm_inverse;
    stroker->tolerance = tolerance;
    stroker->mesh = mesh;

    stroker->ctm_determinant = _cairo_matrix_compute_determinant (stroker->ctm);
    stroker->ctm_det_positive = stroker->ctm_determinant >= 0.0;

    status = _cairo_pen_init (&stroker->pen,
		              stroke_style->line_width / 2.0,
			      tolerance, ctm);
    if (unlikely (status))
	return status;

    stroker->has_current_face = FALSE;
    stroker->has_first_face = FALSE;
    stroker->has_initial_sub_path = FALSE;

    if (stroker->style->dash)
	_cairo_stroker_start_dash (stroker);
    else
	stroker->dashed = FALSE;

    return CAIRO_STATUS_SUCCESS;
}

static void
_cairo_stroker_fini (cairo_stroker_t *stroker)
{
    _cairo_pen_fini (&stroker->pen);
}

static void
_translate_point (cairo_point_t *point, cairo_point_t *offset)
{
    point->x += offset->x;
    point->y += offset->y;
}

static int
_cairo_stroker_face_clockwise (cairo_stroke_face_t *in, cairo_stroke_face_t *out)
{
    cairo_slope_t in_slope, out_slope;

    _cairo_slope_init (&in_slope, &in->point, &in->cw);
    _cairo_slope_init (&out_slope, &out->point, &out->cw);

    return _cairo_slope_compare (&in_slope, &out_slope) < 0;
}

/**
 * _cairo_slope_compare_sgn
 *
 * Return -1, 0 or 1 depending on the relative slopes of
 * two lines.
 */
static int
_cairo_slope_compare_sgn (double dx1, double dy1, double dx2, double dy2)
{
    double  c = (dx1 * dy2 - dx2 * dy1);

    if (c > 0) return 1;
    if (c < 0) return -1;
    return 0;
}

static cairo_status_t
_cairo_stroker_join (cairo_stroker_t *stroker, cairo_stroke_face_t *in, cairo_stroke_face_t *out)
{
    int			clockwise = _cairo_stroker_face_clockwise (out, in);
    cairo_point_t	*inpt, *outpt;
    cairo_status_t status;

    if (in->cw.x == out->cw.x
	&& in->cw.y == out->cw.y
	&& in->ccw.x == out->ccw.x
	&& in->ccw.y == out->ccw.y)
    {
	return CAIRO_STATUS_SUCCESS;
    }

    if (clockwise) {
    	inpt = &in->ccw;
    	outpt = &out->ccw;
    } else {
    	inpt = &in->cw;
    	outpt = &out->cw;
    }

    switch (stroker->style->line_join) {
    case CAIRO_LINE_JOIN_ROUND: {
	int i;
	int start, step, stop;
	cairo_point_t tri[3];
	cairo_pen_t *pen = &stroker->pen;

	tri[0] = in->point;
	if (clockwise) {
	    start =
		_cairo_pen_find_active_ccw_vertex_index (pen, &in->dev_vector);
	    stop =
		_cairo_pen_find_active_ccw_vertex_index (pen, &out->dev_vector);
	    step = -1;
	} else {
	    start =
		_cairo_pen_find_active_cw_vertex_index (pen, &in->dev_vector);
	    stop =
		_cairo_pen_find_active_cw_vertex_index (pen, &out->dev_vector);
	    step = +1;
	}

	i = start;
	tri[1] = *inpt;
	while (i != stop) {
	    tri[2] = in->point;
	    _translate_point (&tri[2], &pen->vertices[i].point);
            status = _cairo_gral_path_stroke_triangle (stroker->mesh, tri);
	    if (status)
		return status;
	    tri[1] = tri[2];
	    i += step;
	    if (i < 0)
		i = pen->num_vertices - 1;
	    if (i >= pen->num_vertices)
		i = 0;
	}

	tri[2] = *outpt;

	return _cairo_gral_path_stroke_triangle (stroker->mesh, tri);
    }
    case CAIRO_LINE_JOIN_MITER:
    default: {
	/* dot product of incoming slope vector with outgoing slope vector */
	double	in_dot_out = ((-in->usr_vector.x * out->usr_vector.x)+
			      (-in->usr_vector.y * out->usr_vector.y));
	double	ml = stroker->style->miter_limit;

	/* Check the miter limit -- lines meeting at an acute angle
	 * can generate long miters, the limit converts them to bevel
	 *
	 * Consider the miter join formed when two line segments
	 * meet at an angle psi:
	 *
	 *	   /.\
	 *	  /. .\
	 *	 /./ \.\
	 *	/./psi\.\
	 *
	 * We can zoom in on the right half of that to see:
	 *
	 *	    |\
	 *	    | \ psi/2
	 *	    |  \
	 *	    |   \
	 *	    |    \
	 *	    |     \
	 * 	  miter    \
	 *	 length     \
	 *	    |        \
	 *	    |        .\
	 *	    |    .     \
	 *	    |.   line   \
	 *	     \    width  \
	 *	      \           \
	 *
	 *
	 * The right triangle in that figure, (the line-width side is
	 * shown faintly with three '.' characters), gives us the
	 * following expression relating miter length, angle and line
	 * width:
	 *
	 *	1 /sin (psi/2) = miter_length / line_width
	 *
	 * The right-hand side of this relationship is the same ratio
	 * in which the miter limit (ml) is expressed. We want to know
	 * when the miter length is within the miter limit. That is
	 * when the following condition holds:
	 *
	 *	1/sin(psi/2) <= ml
	 *	1 <= ml sin(psi/2)
	 *	1 <= ml² sin²(psi/2)
	 *	2 <= ml² 2 sin²(psi/2)
	 *				2·sin²(psi/2) = 1-cos(psi)
	 *	2 <= ml² (1-cos(psi))
	 *
	 *				in · out = |in| |out| cos (psi)
	 *
	 * in and out are both unit vectors, so:
	 *
	 *				in · out = cos (psi)
	 *
	 *	2 <= ml² (1 - in · out)
	 *
	 */
	if (2 <= ml * ml * (1 - in_dot_out))
	{
	    double		x1, y1, x2, y2;
	    double		mx, my;
	    double		dx1, dx2, dy1, dy2;
	    cairo_point_t	outer;
	    cairo_point_t	quad[4];
	    double		ix, iy;
	    double		fdx1, fdy1, fdx2, fdy2;
	    double		mdx, mdy;

	    /*
	     * we've got the points already transformed to device
	     * space, but need to do some computation with them and
	     * also need to transform the slope from user space to
	     * device space
	     */
	    /* outer point of incoming line face */
	    x1 = _cairo_fixed_to_double (inpt->x);
	    y1 = _cairo_fixed_to_double (inpt->y);
	    dx1 = in->usr_vector.x;
	    dy1 = in->usr_vector.y;
	    cairo_matrix_transform_distance (stroker->ctm, &dx1, &dy1);

	    /* outer point of outgoing line face */
	    x2 = _cairo_fixed_to_double (outpt->x);
	    y2 = _cairo_fixed_to_double (outpt->y);
	    dx2 = out->usr_vector.x;
	    dy2 = out->usr_vector.y;
	    cairo_matrix_transform_distance (stroker->ctm, &dx2, &dy2);

	    /*
	     * Compute the location of the outer corner of the miter.
	     * That's pretty easy -- just the intersection of the two
	     * outer edges.  We've got slopes and points on each
	     * of those edges.  Compute my directly, then compute
	     * mx by using the edge with the larger dy; that avoids
	     * dividing by values close to zero.
	     */
	    my = (((x2 - x1) * dy1 * dy2 - y2 * dx2 * dy1 + y1 * dx1 * dy2) /
		  (dx1 * dy2 - dx2 * dy1));
	    if (fabs (dy1) >= fabs (dy2))
		mx = (my - y1) * dx1 / dy1 + x1;
	    else
		mx = (my - y2) * dx2 / dy2 + x2;

	    /*
	     * When the two outer edges are nearly parallel, slight
	     * perturbations in the position of the outer points of the lines
	     * caused by representing them in fixed point form can cause the
	     * intersection point of the miter to move a large amount. If
	     * that moves the miter intersection from between the two faces,
	     * then draw a bevel instead.
	     */

	    ix = _cairo_fixed_to_double (in->point.x);
	    iy = _cairo_fixed_to_double (in->point.y);

	    /* slope of one face */
	    fdx1 = x1 - ix; fdy1 = y1 - iy;

	    /* slope of the other face */
	    fdx2 = x2 - ix; fdy2 = y2 - iy;

	    /* slope from the intersection to the miter point */
	    mdx = mx - ix; mdy = my - iy;

	    /*
	     * Make sure the miter point line lies between the two
	     * faces by comparing the slopes
	     */
	    if (_cairo_slope_compare_sgn (fdx1, fdy1, mdx, mdy) !=
		_cairo_slope_compare_sgn (fdx2, fdy2, mdx, mdy))
	    {
		/*
		 * Draw the quadrilateral
		 */
		outer.x = _cairo_fixed_from_double (mx);
		outer.y = _cairo_fixed_from_double (my);

		quad[0] = in->point;
		quad[1] = *inpt;
		quad[2] = outer;
		quad[3] = *outpt;

		return _cairo_gral_path_stroke_convex_quad (stroker->mesh, quad);
	    }
	}
	/* fall through ... */
    }
    case CAIRO_LINE_JOIN_BEVEL: {
	cairo_point_t tri[3];
	tri[0] = in->point;
	tri[1] = *inpt;
	tri[2] = *outpt;

	return _cairo_gral_path_stroke_triangle (stroker->mesh, tri);
    }
    }
}

static cairo_status_t
_cairo_stroker_add_cap (cairo_stroker_t *stroker, cairo_stroke_face_t *f)
{
    cairo_status_t	    status;

    if (stroker->style->line_cap == CAIRO_LINE_CAP_BUTT)
	return CAIRO_STATUS_SUCCESS;

    switch (stroker->style->line_cap) {
    case CAIRO_LINE_CAP_ROUND: {
	int i;
	int start, stop;
	cairo_slope_t slope;
	cairo_point_t tri[3];
	cairo_pen_t *pen = &stroker->pen;

	slope = f->dev_vector;
	start = _cairo_pen_find_active_cw_vertex_index (pen, &slope);
	slope.dx = -slope.dx;
	slope.dy = -slope.dy;
	stop = _cairo_pen_find_active_cw_vertex_index (pen, &slope);

	tri[0] = f->point;
	tri[1] = f->cw;
	for (i=start; i != stop; i = (i+1) % pen->num_vertices) {
	    tri[2] = f->point;
	    _translate_point (&tri[2], &pen->vertices[i].point);
            status = _cairo_gral_path_stroke_triangle (stroker->mesh, tri);
	    if (status)
		return status;
	    tri[1] = tri[2];
	}
	tri[2] = f->ccw;

	return _cairo_gral_path_stroke_triangle (stroker->mesh, tri);
    }
    case CAIRO_LINE_CAP_SQUARE: {
	double dx, dy;
	cairo_slope_t	fvector;
	cairo_point_t	occw, ocw;
	cairo_point_t	quad[4];

	dx = f->usr_vector.x;
	dy = f->usr_vector.y;
	dx *= stroker->style->line_width / 2.0;
	dy *= stroker->style->line_width / 2.0;
	cairo_matrix_transform_distance (stroker->ctm, &dx, &dy);
	fvector.dx = _cairo_fixed_from_double (dx);
	fvector.dy = _cairo_fixed_from_double (dy);
	occw.x = f->ccw.x + fvector.dx;
	occw.y = f->ccw.y + fvector.dy;
	ocw.x = f->cw.x + fvector.dx;
	ocw.y = f->cw.y + fvector.dy;

        quad[0] = f->cw;
        quad[1] = ocw;
        quad[2] = occw;
        quad[3] = f->ccw;

	return _cairo_gral_path_stroke_convex_quad (stroker->mesh, quad);
    }
    case CAIRO_LINE_CAP_BUTT:
    default:
	return CAIRO_STATUS_SUCCESS;
    }
}

static cairo_status_t
_cairo_stroker_add_leading_cap (cairo_stroker_t     *stroker,
				cairo_stroke_face_t *face)
{
    cairo_stroke_face_t reversed;
    cairo_point_t t;

    reversed = *face;

    /* The initial cap needs an outward facing vector. Reverse everything */
    reversed.usr_vector.x = -reversed.usr_vector.x;
    reversed.usr_vector.y = -reversed.usr_vector.y;
    reversed.dev_vector.dx = -reversed.dev_vector.dx;
    reversed.dev_vector.dy = -reversed.dev_vector.dy;
    t = reversed.cw;
    reversed.cw = reversed.ccw;
    reversed.ccw = t;

    return _cairo_stroker_add_cap (stroker, &reversed);
}

static cairo_status_t
_cairo_stroker_add_trailing_cap (cairo_stroker_t     *stroker,
				 cairo_stroke_face_t *face)
{
    return _cairo_stroker_add_cap (stroker, face);
}

static inline cairo_bool_t
_compute_normalized_device_slope (double *dx, double *dy, cairo_matrix_t *ctm_inverse, double *mag_out)
{
    double dx0 = *dx, dy0 = *dy;
    double mag;

    cairo_matrix_transform_distance (ctm_inverse, &dx0, &dy0);

    if (dx0 == 0.0 && dy0 == 0.0) {
	if (mag_out)
	    *mag_out = 0.0;
	return FALSE;
    }

    if (dx0 == 0.0) {
	*dx = 0.0;
	if (dy0 > 0.0) {
	    mag = dy0;
	    *dy = 1.0;
	} else {
	    mag = -dy0;
	    *dy = -1.0;
	}
    } else if (dy0 == 0.0) {
	*dy = 0.0;
	if (dx0 > 0.0) {
	    mag = dx0;
	    *dx = 1.0;
	} else {
	    mag = -dx0;
	    *dx = -1.0;
	}
    } else {
	mag = sqrt (dx0 * dx0 + dy0 * dy0);
	*dx = dx0 / mag;
	*dy = dy0 / mag;
    }

    if (mag_out)
	*mag_out = mag;

    return TRUE;
}

static void
_compute_face (const cairo_point_t *point, cairo_slope_t *dev_slope,
	       double slope_dx, double slope_dy,
	       cairo_stroker_t *stroker, cairo_stroke_face_t *face);

static cairo_status_t
_cairo_stroker_add_caps (cairo_stroker_t *stroker)
{
    cairo_status_t status;
    /* check for a degenerative sub_path */
    if (stroker->has_initial_sub_path
	&& !stroker->has_first_face
	&& !stroker->has_current_face
	&& stroker->style->line_cap == CAIRO_LINE_JOIN_ROUND)
    {
	/* pick an arbitrary slope to use */
	double dx = 1.0, dy = 0.0;
	cairo_slope_t slope = { CAIRO_FIXED_ONE, 0 };
	cairo_stroke_face_t face;

	_compute_normalized_device_slope (&dx, &dy, stroker->ctm_inverse, NULL);

	/* arbitrarily choose first_point
	 * first_point and current_point should be the same */
	_compute_face (&stroker->first_point, &slope, dx, dy, stroker, &face);

	status = _cairo_stroker_add_leading_cap (stroker, &face);
	if (unlikely (status))
	    return status;
	status = _cairo_stroker_add_trailing_cap (stroker, &face);
	if (unlikely (status))
	    return status;
    }

    if (stroker->has_first_face) {
	status = _cairo_stroker_add_leading_cap (stroker, &stroker->first_face);
	if (unlikely (status))
	    return status;
    }

    if (stroker->has_current_face) {
	status = _cairo_stroker_add_trailing_cap (stroker, &stroker->current_face);
	if (unlikely (status))
	    return status;
    }

    return CAIRO_STATUS_SUCCESS;
}

static void
_compute_face (const cairo_point_t *point, cairo_slope_t *dev_slope,
	       double slope_dx, double slope_dy,
	       cairo_stroker_t *stroker, cairo_stroke_face_t *face)
{
    double face_dx, face_dy;
    cairo_point_t offset_ccw, offset_cw;

    /*
     * rotate to get a line_width/2 vector along the face, note that
     * the vector must be rotated the right direction in device space,
     * but by 90° in user space. So, the rotation depends on
     * whether the ctm reflects or not, and that can be determined
     * by looking at the determinant of the matrix.
     */
    if (stroker->ctm_det_positive)
    {
	face_dx = - slope_dy * (stroker->style->line_width / 2.0);
	face_dy = slope_dx * (stroker->style->line_width / 2.0);
    }
    else
    {
	face_dx = slope_dy * (stroker->style->line_width / 2.0);
	face_dy = - slope_dx * (stroker->style->line_width / 2.0);
    }

    /* back to device space */
    cairo_matrix_transform_distance (stroker->ctm, &face_dx, &face_dy);

    offset_ccw.x = _cairo_fixed_from_double (face_dx);
    offset_ccw.y = _cairo_fixed_from_double (face_dy);
    offset_cw.x = -offset_ccw.x;
    offset_cw.y = -offset_ccw.y;

    face->ccw = *point;
    _translate_point (&face->ccw, &offset_ccw);

    face->point = *point;

    face->cw = *point;
    _translate_point (&face->cw, &offset_cw);

    face->usr_vector.x = slope_dx;
    face->usr_vector.y = slope_dy;

    face->dev_vector = *dev_slope;
}

static cairo_status_t
_cairo_stroker_add_sub_edge (cairo_stroker_t *stroker,
			     const cairo_point_t *p1,
			     const cairo_point_t *p2,
			     cairo_slope_t *dev_slope,
			     double slope_dx, double slope_dy,
			     cairo_stroke_face_t *start,
			     cairo_stroke_face_t *end)
{
    cairo_point_t rectangle[4];

    _compute_face (p1, dev_slope, slope_dx, slope_dy, stroker, start);

    /* XXX: This could be optimized slightly by not calling
       _compute_face again but rather  translating the relevant
       fields from start. */
    _compute_face (p2, dev_slope, slope_dx, slope_dy, stroker, end);

    if (p1->x == p2->x && p1->y == p2->y)
	return CAIRO_STATUS_SUCCESS;

    rectangle[0] = start->cw;
    rectangle[1] = start->ccw;
    rectangle[2] = end->ccw;
    rectangle[3] = end->cw;

    return _cairo_gral_path_stroke_convex_quad (stroker->mesh, rectangle);
}

static cairo_status_t
_cairo_stroker_move_to (void *closure,
			const cairo_point_t *point)
{
    cairo_status_t status;
    cairo_stroker_t *stroker = closure;

    /* Cap the start and end of the previous sub path as needed */
    status = _cairo_stroker_add_caps (stroker);
    if (unlikely (status))
	return status;

    stroker->first_point = *point;
    stroker->current_point = *point;

    stroker->has_first_face = FALSE;
    stroker->has_current_face = FALSE;
    stroker->has_initial_sub_path = FALSE;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_stroker_move_to_dashed (void *closure,
			       const cairo_point_t *point)
{
    /* reset the dash pattern for new sub paths */
    cairo_stroker_t *stroker = closure;
    _cairo_stroker_start_dash (stroker);

    return _cairo_stroker_move_to (closure, point);
}

static cairo_status_t
_cairo_stroker_line_to (void *closure,
			const cairo_point_t *p2)
{
    cairo_status_t status;
    cairo_stroker_t *stroker = closure;
    cairo_stroke_face_t start, end;
    cairo_point_t *p1 = &stroker->current_point;
    cairo_slope_t dev_slope;
    double slope_dx, slope_dy;

    stroker->has_initial_sub_path = TRUE;

    if (p1->x == p2->x && p1->y == p2->y)
	return CAIRO_STATUS_SUCCESS;

    _cairo_slope_init (&dev_slope, p1, p2);
    slope_dx = _cairo_fixed_to_double (p2->x - p1->x);
    slope_dy = _cairo_fixed_to_double (p2->y - p1->y);
    _compute_normalized_device_slope (&slope_dx, &slope_dy, stroker->ctm_inverse, NULL);

    status = _cairo_stroker_add_sub_edge (stroker,
					  p1, p2,
					  &dev_slope,
					  slope_dx, slope_dy,
					  &start, &end);
    if (unlikely (status))
	return status;

    if (stroker->has_current_face) {
	/* Join with final face from previous segment */
	status = _cairo_stroker_join (stroker, &stroker->current_face, &start);
	if (unlikely (status))
	    return status;
    } else if (!stroker->has_first_face) {
	/* Save sub path's first face in case needed for closing join */
	stroker->first_face = start;
	stroker->has_first_face = TRUE;
    }
    stroker->current_face = end;
    stroker->has_current_face = TRUE;

    stroker->current_point = *p2;

    return CAIRO_STATUS_SUCCESS;
}

/*
 * Dashed lines.  Cap each dash end, join around turns when on
 */
static cairo_status_t
_cairo_stroker_line_to_dashed (void *closure,
			       const cairo_point_t *p2)
{
    cairo_stroker_t *stroker = closure;
    double mag, remain, step_length = 0;
    double slope_dx, slope_dy;
    double dx2, dy2;
    cairo_stroke_face_t sub_start, sub_end;
    cairo_point_t *p1 = &stroker->current_point;
    cairo_slope_t dev_slope;
    cairo_line_t segment;
    cairo_status_t status;

    stroker->has_initial_sub_path = stroker->dash_starts_on;

    if (p1->x == p2->x && p1->y == p2->y)
	return CAIRO_STATUS_SUCCESS;

    _cairo_slope_init (&dev_slope, p1, p2);

    slope_dx = _cairo_fixed_to_double (p2->x - p1->x);
    slope_dy = _cairo_fixed_to_double (p2->y - p1->y);

    if (! _compute_normalized_device_slope (&slope_dx, &slope_dy,
					   stroker->ctm_inverse, &mag))
    {
	return CAIRO_STATUS_SUCCESS;
    }

    remain = mag;
    segment.p1 = *p1;
    while (remain) {
	step_length = MIN (stroker->dash_remain, remain);
	remain -= step_length;
	dx2 = slope_dx * (mag - remain);
	dy2 = slope_dy * (mag - remain);
	cairo_matrix_transform_distance (stroker->ctm, &dx2, &dy2);
	segment.p2.x = _cairo_fixed_from_double (dx2) + p1->x;
	segment.p2.y = _cairo_fixed_from_double (dy2) + p1->y;

	if (stroker->dash_on)
	{
	    status = _cairo_stroker_add_sub_edge (stroker,
						  &segment.p1, &segment.p2,
						  &dev_slope,
						  slope_dx, slope_dy,
						  &sub_start, &sub_end);
	    if (unlikely (status))
		return status;

	    if (stroker->has_current_face) {
		/* Join with final face from previous segment */
		status = _cairo_stroker_join (stroker,
					      &stroker->current_face,
					      &sub_start);
		if (unlikely (status))
		    return status;

		stroker->has_current_face = FALSE;
	    } else if (! stroker->has_first_face && stroker->dash_starts_on) {
		/* Save sub path's first face in case needed for closing join */
		stroker->first_face = sub_start;
		stroker->has_first_face = TRUE;
	    } else {
		/* Cap dash start if not connecting to a previous segment */
		status = _cairo_stroker_add_leading_cap (stroker, &sub_start);
		if (unlikely (status))
		    return status;
	    }

	    if (remain) {
		/* Cap dash end if not at end of segment */
		status = _cairo_stroker_add_trailing_cap (stroker, &sub_end);
		if (unlikely (status))
		    return status;
	    } else {
		stroker->current_face = sub_end;
		stroker->has_current_face = TRUE;
	    }
	} else {
	    if (stroker->has_current_face) {
		/* Cap final face from previous segment */
		status = _cairo_stroker_add_trailing_cap (stroker,
							  &stroker->current_face);
		if (unlikely (status))
		    return status;

		stroker->has_current_face = FALSE;
	    }
	}

	_cairo_stroker_step_dash (stroker, step_length);
	segment.p1 = segment.p2;
    }

    if (stroker->dash_on && ! stroker->has_current_face) {
	/* This segment ends on a transition to dash_on, compute a new face
	 * and add cap for the beginning of the next dash_on step.
	 *
	 * Note: this will create a degenerate cap if this is not the last line
	 * in the path. Whether this behaviour is desirable or not is debatable.
	 * On one side these degenerate caps can not be reproduced with regular
	 * path stroking.
	 * On the other hand, Acroread 7 also produces the degenerate caps.
	 */
	_compute_face (p2, &dev_slope,
		       slope_dx, slope_dy,
		       stroker,
		       &stroker->current_face);

	status = _cairo_stroker_add_leading_cap (stroker,
						 &stroker->current_face);
	if (unlikely (status))
	    return status;

	stroker->has_current_face = TRUE;
    }

    stroker->current_point = *p2;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_stroker_curve_to (void *closure,
			 const cairo_point_t *b,
			 const cairo_point_t *c,
			 const cairo_point_t *d)
{
    cairo_stroker_t *stroker = closure;
    cairo_gral_pen_stroke_spline_t spline_pen;
    cairo_stroke_face_t start, end;
    cairo_point_t extra_points[4];
    cairo_point_t *a = &stroker->current_point;
    double initial_slope_dx, initial_slope_dy;
    double final_slope_dx, final_slope_dy;
    cairo_status_t status;

    status = _cairo_gral_pen_stroke_spline_init (&spline_pen,
					    &stroker->pen,
					    a, b, c, d, stroker->mesh);
    if (status == CAIRO_INT_STATUS_DEGENERATE)
	return _cairo_stroker_line_to (closure, d);
    else if (unlikely (status))
	return status;

    initial_slope_dx = _cairo_fixed_to_double (spline_pen.spline.initial_slope.dx);
    initial_slope_dy = _cairo_fixed_to_double (spline_pen.spline.initial_slope.dy);
    final_slope_dx = _cairo_fixed_to_double (spline_pen.spline.final_slope.dx);
    final_slope_dy = _cairo_fixed_to_double (spline_pen.spline.final_slope.dy);

    if (_compute_normalized_device_slope (&initial_slope_dx, &initial_slope_dy,
					  stroker->ctm_inverse, NULL))
    {
	_compute_face (a,
		       &spline_pen.spline.initial_slope,
		       initial_slope_dx, initial_slope_dy,
		       stroker, &start);
    }

    if (_compute_normalized_device_slope (&final_slope_dx, &final_slope_dy,
					  stroker->ctm_inverse, NULL))
    {
	_compute_face (d,
		       &spline_pen.spline.final_slope,
		       final_slope_dx, final_slope_dy,
		       stroker, &end);
    }

    if (stroker->has_current_face) {
	status = _cairo_stroker_join (stroker, &stroker->current_face, &start);
	if (unlikely (status))
	    goto CLEANUP_PEN;
    } else if (! stroker->has_first_face) {
	stroker->first_face = start;
	stroker->has_first_face = TRUE;
    }
    stroker->current_face = end;
    stroker->has_current_face = TRUE;

    extra_points[0] = start.cw;
    extra_points[0].x -= start.point.x;
    extra_points[0].y -= start.point.y;
    extra_points[1] = start.ccw;
    extra_points[1].x -= start.point.x;
    extra_points[1].y -= start.point.y;
    extra_points[2] = end.cw;
    extra_points[2].x -= end.point.x;
    extra_points[2].y -= end.point.y;
    extra_points[3] = end.ccw;
    extra_points[3].x -= end.point.x;
    extra_points[3].y -= end.point.y;

    status = _cairo_pen_add_points (&spline_pen.pen, extra_points, 4);
    if (unlikely (status))
	goto CLEANUP_PEN;

    status = _cairo_gral_pen_stroke_spline (&spline_pen,
				            stroker->tolerance);

  CLEANUP_PEN:
    _cairo_gral_pen_stroke_spline_fini (&spline_pen);

    stroker->current_point = *d;

    return status;
}

/* We're using two different algorithms here for dashed and un-dashed
 * splines. The dashed algorithm uses the existing line dashing
 * code. It's linear in path length, but gets subtly wrong results for
 * self-intersecting paths (an outstanding but for self-intersecting
 * non-curved paths as well). The non-dashed algorithm tessellates a
 * single polygon for the whole curve. It handles the
 * self-intersecting problem, but it's (unsurprisingly) not O(n) and
 * more significantly, it doesn't yet handle dashes.
 *
 * The only reason we're doing split algorithms here is to
 * minimize the impact of fixing the splines-aren't-dashed bug for
 * 1.0.2. Long-term the right answer is to rewrite the whole pile
 * of stroking code so that the entire result is computed as a
 * single polygon that is tessellated, (that is, stroking can be
 * built on top of filling). That will solve the self-intersecting
 * problem. It will also increase the importance of implementing
 * an efficient and more robust tessellator.
 */
static cairo_status_t
_cairo_stroker_curve_to_dashed (void *closure,
				const cairo_point_t *b,
				const cairo_point_t *c,
				const cairo_point_t *d)
{
    cairo_stroker_t *stroker = closure;
    cairo_spline_t spline;
    cairo_point_t *a = &stroker->current_point;
    cairo_line_join_t line_join_save;
    cairo_status_t status;

    if (! _cairo_spline_init (&spline,
			      stroker->dashed ?
			      _cairo_stroker_line_to_dashed :
			      _cairo_stroker_line_to,
			      stroker,
			      a, b, c, d))
    {
	return _cairo_stroker_line_to_dashed (closure, d);
    }

    /* If the line width is so small that the pen is reduced to a
       single point, then we have nothing to do. */
    if (stroker->pen.num_vertices <= 1)
	return CAIRO_STATUS_SUCCESS;

    /* Temporarily modify the stroker to use round joins to guarantee
     * smooth stroked curves. */
    line_join_save = stroker->style->line_join;
    stroker->style->line_join = CAIRO_LINE_JOIN_ROUND;

    status = _cairo_spline_decompose (&spline, stroker->tolerance);

    stroker->style->line_join = line_join_save;

    return status;
}

static cairo_status_t
_cairo_stroker_close_path (void *closure)
{
    cairo_status_t status;
    cairo_stroker_t *stroker = closure;

    if (stroker->dashed)
	status = _cairo_stroker_line_to_dashed (stroker, &stroker->first_point);
    else
	status = _cairo_stroker_line_to (stroker, &stroker->first_point);
    if (unlikely (status))
	return status;

    if (stroker->has_first_face && stroker->has_current_face) {
	/* Join first and final faces of sub path */
	status = _cairo_stroker_join (stroker, &stroker->current_face, &stroker->first_face);
	if (unlikely (status))
	    return status;
    } else {
	/* Cap the start and end of the sub path as needed */
	status = _cairo_stroker_add_caps (stroker);
	if (unlikely (status))
	    return status;
    }

    stroker->has_initial_sub_path = FALSE;
    stroker->has_first_face = FALSE;
    stroker->has_current_face = FALSE;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_gral_path_fixed_stroke_to_mesh (cairo_path_fixed_t	*path,
				   cairo_stroke_style_t	*stroke_style,
				   cairo_matrix_t	*ctm,
				   cairo_matrix_t	*ctm_inverse,
				   double		 tolerance,
				   cairo_gral_stroke_path_mesh_t *mesh)
{
    cairo_status_t status;
    cairo_stroker_t stroker;

    status = _cairo_stroker_init (&stroker, stroke_style,
			          ctm, ctm_inverse, tolerance,
				  mesh);
    if (unlikely (status))
	return status;

    if (stroker.style->dash)
	status = _cairo_path_fixed_interpret (path,
					      CAIRO_DIRECTION_FORWARD,
					      _cairo_stroker_move_to_dashed,
					      _cairo_stroker_line_to_dashed,
					      _cairo_stroker_curve_to_dashed,
					      _cairo_stroker_close_path,
					      &stroker);
    else
	status = _cairo_path_fixed_interpret (path,
					      CAIRO_DIRECTION_FORWARD,
					      _cairo_stroker_move_to,
					      _cairo_stroker_line_to,
					      _cairo_stroker_curve_to,
					      _cairo_stroker_close_path,
					      &stroker);
    if (unlikely (status))
	goto BAIL;

    /* Cap the start and end of the final sub path as needed */
    status = _cairo_stroker_add_caps (&stroker);

BAIL:
    _cairo_stroker_fini (&stroker);

    return status;
}
