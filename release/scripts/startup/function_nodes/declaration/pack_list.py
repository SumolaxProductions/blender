import bpy
import uuid
from bpy.props import *
from . base import SocketDeclBase
from .. base import DataSocket
from .. types import type_infos
from .. sockets import OperatorSocket

class PackListDecl(SocketDeclBase):
    def __init__(self, node, identifier: str, prop_name: str, base_type: str, default_amount: int):
        self.node = node
        self.identifier_suffix = identifier
        self.prop_name = prop_name
        self.base_type = base_type
        self.list_type = type_infos.to_list(base_type)
        self.default_amount = default_amount

    def init(self):
        collection = self.get_collection()
        for _ in range(self.default_amount):
            item = collection.add()
            item.state = "BASE"
            item.identifier_prefix = str(uuid.uuid4())

    def build(self, node_sockets):
        return list(self._build(node_sockets))

    def _build(self, node_sockets):
        for item in self.get_collection():
            data_type = self.base_type if item.state == "BASE" else self.list_type
            yield type_infos.build(
                data_type,
                node_sockets,
                "",
                item.identifier_prefix + self.identifier_suffix)
        yield node_sockets.new("fn_OperatorSocket", "Operator")

    def validate(self, sockets):
        collection = self.get_collection()
        if len(sockets) != len(collection) + 1:
            return False

        for socket, item in zip(sockets[:-1], collection):
            data_type = self.base_type if item.state == "BASE" else self.list_type
            identifier = item.identifier_prefix + self.identifier_suffix
            if not self._data_socket_test(socket, "", data_type, identifier):
                return False

        if sockets[-1].bl_idname != "fn_OperatorSocket":
            return False

        return True

    def draw_socket(self, layout, socket, index):
        if isinstance(socket, OperatorSocket):
            props = layout.operator("fn.new_pack_list_input", text="New Input", icon='ADD')
            props.tree_name = self.node.tree.name
            props.node_name = self.node.name
            props.prop_name = self.prop_name
        else:
            row = layout.row(align=True)
            socket.draw_self(row, self.node, str(index))
            props = row.operator("fn.remove_pack_list_input", text="", icon='X')
            props.tree_name = self.node.tree.name
            props.node_name = self.node.name
            props.prop_name = self.prop_name
            props.index = index

    def operator_socket_call(self, own_socket, linked_socket, connected_sockets):
        if len(connected_sockets) != 1:
            return
        connected_socket = next(iter(connected_sockets))
        if not isinstance(connected_socket, DataSocket):
            return

        is_output = own_socket.is_output
        data_type = connected_socket.data_type

        if type_infos.is_base(data_type):
            if not type_infos.is_link_allowed(data_type, self.base_type):
                return
            state = "BASE"
        elif type_infos.is_list(data_type):
            if not type_infos.is_link_allowed(data_type, self.list_type):
                return
            state = "LIST"
        else:
            return

        collection = self.get_collection()
        item = collection.add()
        item.state = state
        item.identifier_prefix = str(uuid.uuid4())

        self.node.rebuild()

        identifier = item.identifier_prefix + self.identifier_suffix
        new_socket = self.node.find_socket(identifier, is_output)
        self.node.tree.new_link(linked_socket, new_socket)

    def amount(self):
        return len(self.get_collection()) + 1

    def get_collection(self):
        return getattr(self.node, self.prop_name)

    @classmethod
    def Property(cls):
        return CollectionProperty(type=PackListPropertyGroup)

class PackListPropertyGroup(bpy.types.PropertyGroup):
    bl_idname = "fn_PackListPropertyGroup"

    state: EnumProperty(
        default="BASE",
        items=[
            ("BASE", "Base", "", "NONE", 0),
            ("LIST", "Base", "", "NONE", 1)])
    identifier_prefix: StringProperty()

class NewPackListInputOperator(bpy.types.Operator):
    bl_idname = "fn.new_pack_list_input"
    bl_label = "New Pack List Input"
    bl_options = {'INTERNAL'}

    tree_name: StringProperty()
    node_name: StringProperty()
    prop_name: StringProperty()

    def execute(self, context):
        tree = bpy.data.node_groups[self.tree_name]
        node = tree.nodes[self.node_name]
        collection = getattr(node, self.prop_name)

        item = collection.add()
        item.state = "BASE"
        item.identifier_prefix = str(uuid.uuid4())

        node.refresh()

        return {'FINISHED'}

class RemovePackListInputOperator(bpy.types.Operator):
    bl_idname = "fn.remove_pack_list_input"
    bl_label = "Remove Pack List Input"
    bl_options = {'INTERNAL'}

    tree_name: StringProperty()
    node_name: StringProperty()
    prop_name: StringProperty()
    index: IntProperty()

    def execute(self, context):
        tree = bpy.data.node_groups[self.tree_name]
        node = tree.nodes[self.node_name]
        collection = getattr(node, self.prop_name)
        collection.remove(self.index)
        node.refresh()
        return {'FINISHED'}
